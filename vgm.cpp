#include "base.h"
#include "reader.h"
#include "config.h"

#include <gme/Vgm_Emu.h>

#include <fex/Gzip_Reader.h>

static const char tag_track_name[]    = "title";
static const char tag_track_name_e[]  = "title_e";
static const char tag_track_name_j[]  = "title_j";
static const char tag_game_name[]     = "album";
static const char tag_game_name_e[]   = "album_e";
static const char tag_game_name_j[]   = "album_j";
static const char tag_system_name[]   = "system";
static const char tag_system_name_e[] = "system_e";
static const char tag_system_name_j[] = "system_j";
static const char tag_artist[]        = "artist";
static const char tag_artist_e[]      = "artist_e";
static const char tag_artist_j[]      = "artist_j";
static const char tag_date[]          = "date";
static const char tag_ripper[]        = "ripper";
static const char tag_notes[]         = "comment";

static void handle_error( const char * str )
{
	if ( str ) throw exception_io_data( str );
}

static void uncompressStream( const service_ptr_t< file > & src_fd, service_ptr_t< file > & dst_fd, abort_callback & p_abort )
{
	foobar_File_Reader in( src_fd, p_abort );
	Gzip_Reader ingz;
	handle_error( ingz.open( &in ) );

	if ( ! ingz.deflated() ) throw exception_io_data( "Not GZIP data" );

	while ( ingz.remain() )
	{
		unsigned char buffer[1024];

		BOOST::uint64_t to_read = ingz.remain();
		if ( to_read > 1024 ) to_read = 1024;
		handle_error( ingz.read( buffer, (long)to_read ) );
		dst_fd->write( buffer, (t_size)to_read, p_abort );
	}
}

enum {
	cmd_gg_stereo       = 0x4F,
	cmd_psg             = 0x50,
	cmd_ym2413          = 0x51,
	cmd_ym2612_port0    = 0x52,
	cmd_ym2612_port1    = 0x53,
	cmd_ym2151          = 0x54,
	cmd_delay           = 0x61,
	cmd_delay_735       = 0x62,
	cmd_delay_882       = 0x63,
	cmd_byte_delay      = 0x64,
	cmd_end             = 0x66,
	cmd_data_block      = 0x67,
	cmd_short_delay     = 0x70,
	cmd_pcm_delay       = 0x80,
	cmd_pcm_seek        = 0xE0,
	
	pcm_block_type      = 0x00,
	ym2612_dac_port     = 0x2A,
	ym2612_dac_pan_port = 0xB6
};

inline int command_len( int command )
{
	static byte const lens [0x10] = {
	// 0 1 2 3 4 5 6 7 8 9 A B C D E F
	   1,1,1,2,2,3,1,1,1,1,3,3,4,4,5,5
	};
	int len = lens [command >> 4];
	return len;
}

inline int get_le16( byte const* p ) { return pfc::byteswap_if_be_t( * ( t_int16 * ) p ); }
inline int get_le32( byte const* p ) { return pfc::byteswap_if_be_t( * ( t_int32 * ) p ); }

void update_fm_rates( byte const* p, size_t size, Vgm_Emu::header_t & header )
{
	byte const* file_end = p + size;
	int data_offset = get_le32( header.data_offset );
	if ( data_offset )
		p += data_offset + offsetof( Vgm_Emu::header_t, data_offset ) - header.size();
	while ( p < file_end )
	{
		switch ( *p )
		{
		case cmd_end:
			return;
		
		case cmd_psg:
		case cmd_byte_delay:
			p += 2;
			break;
		
		case cmd_delay:
			p += 3;
			break;
		
		case cmd_data_block:
			p += 7 + get_le32( p + 3 );
			break;
		
		case cmd_ym2413:
			memset( header.ym2612_rate, 0, sizeof( header.ym2612_rate ) );
			memset( header.ym2151_rate, 0, sizeof( header.ym2151_rate ) );
			return;
		
		case cmd_ym2612_port0:
		case cmd_ym2612_port1:
			memset( header.ym2413_rate, 0, sizeof( header.ym2413_rate ) );
			memset( header.ym2151_rate, 0, sizeof( header.ym2151_rate ) );
			return;
		
		case cmd_ym2151:
			memset( header.ym2612_rate, 0, sizeof( header.ym2612_rate ) );
			memset( header.ym2413_rate, 0, sizeof( header.ym2413_rate ) );
			return;
		
		default:
			p += command_len( *p );
		}
	}
}

inline void set_int( file_info & p_info, const char * p_name, int val )
{
	if ( val ) p_info.info_set_int( p_name, val );
}

inline void set_freq( file_info & p_info, const char * p_chip_name, int freq )
{
	if ( freq )
	{
		pfc::string8 name2;
		pfc::string8 name = "VGM_";
		name += p_chip_name;
		name2 = name;
		name += "_RATE";
		name2 += "_COUNT";
		p_info.info_set_int( name, freq & 0xBFFFFFFF );
		p_info.info_set_int( name2, (freq & 0x40000000) ? 2 : 1 );
	}
}

class input_vgm : public input_gep
{
	double volume_modifier;

	Vgm_Emu::header_t m_header;

	const wchar_t * get_string( const wchar_t * & ptr, unsigned & wchar_remain )
	{
		while ( * ptr && wchar_remain ) ++ptr, --wchar_remain;
		if ( wchar_remain > 1 )
		{
			--wchar_remain;
			return ++ptr;
		}

		return 0;
	}

	void add_tag( file_info & p_info, const char * name, const wchar_t * value )
	{
		if ( value && * value )
		{
			const wchar_t * ptr_lf = wcschr( value, '\n' );
			const wchar_t * ptr_cr = wcschr( value, '\r' );
			if ( ptr_lf || ptr_cr )
			{
				pfc::string8 temp;
				do
				{
					if ( ptr_cr && ptr_lf && ptr_cr < ptr_lf ) ptr_lf = ptr_cr;
					if ( ptr_lf )
					{
						temp += pfc::stringcvt::string_utf8_from_wide( value, ptr_lf - value );
						temp += "\r\n";
						value = ptr_lf;
						if ( value[ 0 ] == '\r' && value[ 1 ] == '\n' ) value += 2;
						else ++value;
						ptr_lf = wcschr( value, '\n' );
						ptr_cr = wcschr( value, '\r' );
					}
				}
				while ( * value && ( ptr_lf || ptr_cr ) );

				if ( * value ) temp += pfc::stringcvt::string_utf8_from_wide( value );

				p_info.meta_add( name, temp );
			}
			else
				p_info.meta_add( name, pfc::stringcvt::string_utf8_from_wide( value ) );
		}
	}

	void process_gd3_tag( const byte * tag, unsigned size, file_info & p_info )
	{
		static const unsigned char signature[] = { 'G', 'd', '3', ' ' };
		static const unsigned char version[]   = {   0,   1,   0,   0 };

		if ( size >= 12 && ! memcmp( tag, signature, 4 ) && ! memcmp( tag + 4, version, 4 ) )
		{
			unsigned data_size = tag[ 8 ] | ( tag[ 9 ] << 8 ) | ( tag[ 10 ] << 16 ) | ( tag[ 11 ] << 24 );
			if ( data_size && size >= data_size + 12 )
			{
				const wchar_t * ptr = ( const wchar_t * ) ( tag + 12 );
				unsigned wchar_remain = data_size / 2;

				const wchar_t * track_name_e  = ptr;
				const wchar_t * track_name_j  = get_string( ptr, wchar_remain );
				const wchar_t * game_name_e   = get_string( ptr, wchar_remain );
				const wchar_t * game_name_j   = get_string( ptr, wchar_remain );
				const wchar_t * system_name_e = get_string( ptr, wchar_remain );
				const wchar_t * system_name_j = get_string( ptr, wchar_remain );
				const wchar_t * artist_e      = get_string( ptr, wchar_remain );
				const wchar_t * artist_j      = get_string( ptr, wchar_remain );
				const wchar_t * date          = get_string( ptr, wchar_remain );
				const wchar_t * ripper        = get_string( ptr, wchar_remain );
				const wchar_t * notes         = get_string( ptr, wchar_remain );

				const wchar_t * track_name  = 0;
				const wchar_t * game_name   = 0;
				const wchar_t * system_name = 0;
				const wchar_t * artist      = 0;

				if ( cfg_vgm_gd3_prefers_japanese )
				{
					track_name  = ( track_name_j && * track_name_j )   ? track_name_j  : track_name_e;
					game_name   = ( game_name_j && * game_name_j )     ? game_name_j   : game_name_e;
					system_name = ( system_name_j && * system_name_j ) ? system_name_j : system_name_e;
					artist      = ( artist_j && * artist_j )           ? artist_j      : artist_e;
				}
				else
				{
					track_name  = ( track_name_e && * track_name_e )   ? track_name_e  : track_name_j;
					game_name   = ( game_name_e && * game_name_e )     ? game_name_e   : game_name_j;
					system_name = ( system_name_e && * system_name_e ) ? system_name_e : system_name_j;
					artist      = ( artist_e && * artist_e )           ? artist_e      : artist_j;
				}

				add_tag( p_info, tag_track_name, track_name );
				add_tag( p_info, tag_track_name_e, track_name_e );
				add_tag( p_info, tag_track_name_j, track_name_j );
				add_tag( p_info, tag_game_name, game_name );
				add_tag( p_info, tag_game_name_e, game_name_e );
				add_tag( p_info, tag_game_name_j, game_name_j );
				add_tag( p_info, tag_system_name, system_name );
				add_tag( p_info, tag_system_name_e, system_name_e );
				add_tag( p_info, tag_system_name_j, system_name_j );
				add_tag( p_info, tag_artist, artist );
				add_tag( p_info, tag_artist_e, artist_e );
				add_tag( p_info, tag_artist_j, artist_j );
				add_tag( p_info, tag_date, date );
				add_tag( p_info, tag_ripper, ripper );
				add_tag( p_info, tag_notes, notes );
			}
		}
	}

public:
	static bool g_is_our_path( const char * p_path, const char * p_extension )
	{
		if ( ! ( cfg_format_enable & 4 ) ) return false;
		return ! stricmp( p_extension, "vgm" ) || ! stricmp( p_extension, "vgz" );
	}

	void open( service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort )
	{
		if ( p_reason == input_open_info_write ) p_reason = input_open_info_read;

		input_gep::open( p_filehint, p_path, p_reason, p_abort );

		try
		{
			service_ptr_t<file> p_unpackfile;
			unpacker::g_open( p_unpackfile, m_file, p_abort );
			m_file = p_unpackfile;
		}
		catch ( const exception_io_data & )
		{
			try
			{
				m_file->reopen( p_abort );

				service_ptr_t<file> p_unpackfile;
				filesystem::g_open_tempmem( p_unpackfile, p_abort );
				uncompressStream( m_file, p_unpackfile, p_abort );
				p_unpackfile->reopen( p_abort );
				m_file = p_unpackfile;
			}
			catch ( const exception_io_data & )
			{
				m_file->reopen( p_abort );
			}
		}

		foobar_Data_Reader rdr(m_file, p_abort);

		{
			ERRCHK( rdr.read( &m_header, m_header.size_min ) );

			if ( !m_header.valid_tag() )
			{
				console::info("Not a VGM file");
				throw exception_io_data();
			}
			t_uint32 version = get_le32( m_header.version );
			if ( version > 0x0161 )
			{
				console::info("Unsupported VGM format");
				throw exception_io_data();
			}
			if ( ! get_le32( m_header.track_duration ) )
			{
				console::info("Header contains empty track duration");
			}
			if ( m_header.size() > m_header.size_min )
			{
				ERRCHK( rdr.read( &m_header.rf5c68_rate, m_header.size() - m_header.size_min ) );
			}

			m_header.cleanup();

			if ( version < 0x110 )
			{
				size_t body_size = m_file->get_size_ex( p_abort ) - m_header.size();

				pfc::array_t<t_uint8> buffer;
				buffer.set_count( body_size );
				ERRCHK( rdr.read( buffer.get_ptr(), body_size ) );

				update_fm_rates( buffer.get_ptr(), body_size, m_header );
			}

			int TempSLng;
			if (m_header.volume_modifier <= 0xC0)
				TempSLng = m_header.volume_modifier;
			else if (m_header.volume_modifier == (0xC0 + 0x01))
				TempSLng = 0xC0 - 0x100;
			else
				TempSLng = m_header.volume_modifier - 0x100;

			volume_modifier = pow( 2.0, (double)TempSLng / 32.0 );
		}
	}

	void get_info( t_uint32 p_subsong, file_info & p_info, abort_callback & p_abort )
	{
		p_info.info_set("codec", "VGM");
		p_info.info_set( "encoding", "synthesized" );

		//p_info.info_set_int("samplerate", sample_rate);
		p_info.info_set_int("channels", 2);
		p_info.info_set_int("bitspersample", 16);

		Vgm_Emu * emu = ( Vgm_Emu * ) this->emu;
		if ( ! emu )
		{
			emu = new Vgm_Emu();

			try
			{
				m_file->seek( 0, p_abort );
				foobar_Data_Reader rdr( m_file, p_abort );

				ERRCHK( emu->set_sample_rate( sample_rate ) );
				ERRCHK( emu->load( rdr ) );
				handle_warning(emu);
			}
			catch(...)
			{
				if ( emu )
				{
					delete emu;
					emu = NULL;
				}
				throw;
			}
		}

		int song_len = get_le32( m_header.track_duration );
		if ( song_len )
		{
			//int fade_min = ( 512 * 8 * 1000 / 2 + sample_rate - 1 ) / sample_rate;
			int loop_len = get_le32( m_header.loop_duration );
			song_len += loop_len * cfg_vgm_loop_count;
			p_info.set_length( song_len / 44100 + tag_fade_ms / 1000 );
		}

		if ( get_le32( m_header.loop_offset ) )
		{
			unsigned loop_end = get_le32( m_header.track_duration );
			unsigned loop_dur = get_le32( m_header.loop_duration );
			p_info.info_set_int("vgm_loop_start", loop_end - loop_dur);
		}

		int version = get_le32( m_header.version );
		p_info.info_set( "VGM_VERSION", pfc::string_formatter() << pfc::format_int( version >> 8, 0, 16 ) << "." << pfc::format_int( version & 0xFF, 2, 16 ) );

		set_freq( p_info, "YM2413", get_le32( m_header.ym2413_rate ) );
		set_int( p_info, "VGM_FRAME_RATE", get_le32( m_header.frame_rate ) );
		set_freq( p_info, "SN76489", get_le32( m_header.psg_rate ) & 0x7FFFFFFF );
		p_info.info_set_float( "VGM_SN76489_GAIN", ( m_header.psg_rate[3] & 0xC0 ) == 0x40 ? 0.5 : 1.0, 1 );
		if ( get_le32( m_header.psg_rate ) )
		{
			p_info.info_set_int( "VGM_SN76489_FLAGS", m_header.sn76489_flags );
			if ( get_le16( m_header.noise_feedback ) )
				p_info.info_set( "VGM_SN76489_NOISE_FEEDBACK", pfc::format_int( get_le16( m_header.noise_feedback ), 4, 16 ) );
			set_int( p_info, "VGM_SN76589_NOISE_WIDTH", m_header.noise_width );
		}
		set_freq( p_info, "YM2612", get_le32( m_header.ym2612_rate ) );
		set_freq( p_info, "YM2151", get_le32( m_header.ym2151_rate ) );
		set_freq( p_info, "SEGAPCM", get_le32( m_header.segapcm_rate ) );
		set_int( p_info, "VGM_RF5C68_RATE", get_le32( m_header.rf5c68_rate ) );
		set_freq( p_info, "YM2203", get_le32( m_header.ym2203_rate ) );
		set_freq( p_info, "YM2608", get_le32( m_header.ym2608_rate ) );
		set_freq( p_info, "YM2610", get_le32( m_header.ym2610_rate ) & 0x7FFFFFFF );
		if ( get_le32( m_header.ym2610_rate ) )
			p_info.info_set( "VGM_YM2610_TYPE", ( m_header.ym2610_rate[3] & 0x80 ) ? "YM2610B" : "YM2610" );
		set_freq( p_info, "YM3812", get_le32( m_header.ym3812_rate ) );
		set_freq( p_info, "YM3526", get_le32( m_header.ym3526_rate ) );
		set_freq( p_info, "Y8950", get_le32( m_header.y8950_rate ) );
		set_freq( p_info, "YMF262", get_le32( m_header.ymf262_rate ) );
		set_freq( p_info, "YMF278B", get_le32( m_header.ymf278b_rate ) );
		set_freq( p_info, "YMF271", get_le32( m_header.ymf271_rate ) );
		set_freq( p_info, "YMZ280B", get_le32( m_header.ymz280b_rate ) );
		set_int( p_info, "VGM_RF5C164_RATE", get_le32( m_header.rf5c164_rate ) );
		set_int( p_info, "VGM_PWM_RATE", get_le32( m_header.pwm_rate ) );
		set_freq( p_info, "AY8910", get_le32( m_header.ay8910_rate ) );
		if ( get_le32( m_header.ay8910_rate ) || get_le32( m_header.ym2203_rate ) || get_le32( m_header.ym2608_rate ) )
		{
			pfc::string8 ay_type;
			switch(m_header.ay8910_type)
			{
			case 0x00:
				ay_type = "AY-3-8910A";
				break;
			case 0x01:
				ay_type = "AY-3-8912A";
				break;
			case 0x02:
				ay_type = "AY-3-8913A";
				break;
			case 0x03:
				ay_type = "AY8930";
				break;
			case 0x04:
				ay_type = "AY-3-8914";
				break;
			case 0x10:
				ay_type = "YM2149";
				break;
			case 0x11:
				ay_type = "YM3439";
				break;
			case 0x12:
				ay_type = "YMZ284";
				break;
			case 0x13:
				ay_type = "YMZ294";
				break;

			default:
				ay_type = "Unknown (0x";
				ay_type += pfc::format_int( m_header.ay8910_type, 0, 16 );
				ay_type += ")";
			}
			p_info.info_set( "VGM_AY8910_TYPE", ay_type );
			p_info.info_set_int( "VGM_AY8910_FLAGS", m_header.ay8910_flags );
			if ( get_le32( m_header.ym2203_rate ) )
				p_info.info_set_int( "VGM_YM2203_AY8910_FLAGS", m_header.ym2203_ay8910_flags );
			if ( get_le32( m_header.ym2608_rate ) )
				p_info.info_set_int( "VGM_YM2608_AY8910_FLAGS", m_header.ym2608_ay8910_flags );
		}
		if ( volume_modifier != 1.0 )
			p_info.info_set_float( "VGM_VOLUME_MODIFIER", volume_modifier, 3 );
		set_int( p_info, "VGM_LOOP_BASE", m_header.loop_base );
		set_int( p_info, "VGM_LOOP_MODIFIER", m_header.loop_modifier );
		set_freq( p_info, "GBDMG", get_le32( m_header.gbdmg_rate ) );
		set_freq( p_info, "NESAPU", get_le32( m_header.nesapu_rate ) );
		set_freq( p_info, "MULTIPCM", get_le32( m_header.multipcm_rate ) );
		set_freq( p_info, "UPD7759", get_le32( m_header.upd7759_rate ) );
		set_freq( p_info, "OKIM6258", get_le32( m_header.okim6258_rate ) );
		if ( get_le32( m_header.okim6258_rate ) )
			p_info.info_set_int( "VGM_OKIM6258_FLAGS", m_header.okim6258_flags );
		set_freq( p_info, "K054539", get_le32( m_header.k054539_rate ) );
		if ( get_le32( m_header.k054539_rate ) )
			p_info.info_set_int( "VGM_K054539_FLAGS", m_header.k054539_flags );
		set_freq( p_info, "C140", get_le32( m_header.c140_rate ) );
		if ( get_le32( m_header.c140_rate ) )
			p_info.info_set_int( "VGM_C140_TYPE", m_header.c140_type );
		set_freq( p_info, "OKIM6295", get_le32( m_header.okim6295_rate ) & 0x7FFFFFFF );
		if ( get_le32( m_header.okim6295_rate ) )
			p_info.info_set_int( "VGM_OKIM6295_DIVIDER", (m_header.okim6295_rate[3] & 0x80) ? 132 : 165 );
		set_freq( p_info, "K051649", get_le32( m_header.k051649_rate ) );
		set_freq( p_info, "HUC6280", get_le32( m_header.huc6280_rate ) );
		set_freq( p_info, "K053260", get_le32( m_header.k053260_rate ) );
		set_freq( p_info, "POKEY", get_le32( m_header.pokey_rate ) );
		set_freq( p_info, "QSOUND", get_le32( m_header.qsound_rate ) );

		int size = 0;
		const unsigned char * gd3_tag;
		ERRCHK( emu->gd3_data( &gd3_tag, & size ) );
		if ( gd3_tag && size )
		{
			process_gd3_tag( gd3_tag, size, p_info );
		}

		static_api_ptr_t<hasher_md5> p_hasher;
		hasher_md5_state p_hash_state;
		p_hasher->initialize( p_hash_state );
		emu->hash_( info_hasher_md5( p_hasher, p_hash_state ) );
		m_file_hash = p_hasher->get_result( p_hash_state );

		if ( !this->emu ) delete emu;

		pfc::string8 hash_string;

		for ( unsigned i = 0; i < 16; i++ ) hash_string += pfc::format_uint( (t_uint8)m_file_hash.m_data[i], 2, 16 );

		p_info.info_set( "gme_hash", hash_string );

		service_ptr_t<metadb_index_client> index_client = new service_impl_t<metadb_index_client_gep>;
		m_index_hash = index_client->transform( p_info, playable_location_impl( m_path, p_subsong ) );

		pfc::array_t<t_uint8> tag;
		static_api_ptr_t<metadb_index_manager>()->get_user_data_t( guid_gep_index, m_index_hash, tag );

		if ( tag.get_count() )
		{
			file::ptr tag_file;
			filesystem::g_open_tempmem( tag_file, p_abort );
			tag_file->write_object( tag.get_ptr(), tag.get_count(), p_abort );

			p_info.meta_remove_all();

			tag_processor::read_trailing( tag_file, p_info, p_abort );
			p_info.info_set( "tagtype", "apev2 db" );
		}
	}

	void retag_set_info( t_uint32 p_subsong, const file_info & p_info, abort_callback & p_abort )
	{
		file::ptr tag_file;
		filesystem::g_open_tempmem( tag_file, p_abort );
		tag_processor::write_apev2( tag_file, p_info, p_abort );

		pfc::array_t<t_uint8> tag;
		tag_file->seek( 0, p_abort );
		tag.set_count( tag_file->get_size_ex( p_abort ) );
		tag_file->read_object( tag.get_ptr(), tag.get_count(), p_abort );

		static_api_ptr_t<metadb_index_manager>()->set_user_data( guid_gep_index, m_index_hash, tag.get_ptr(), tag.get_count() );
	}

	void retag_commit( abort_callback & p_abort )
	{
	}

	void decode_initialize( t_uint32 p_subsong, unsigned p_flags, abort_callback & p_abort )
	{
		Vgm_Emu * emu = ( Vgm_Emu * ) this->emu;
		if ( ! emu )
		{
			this->emu = emu = new Vgm_Emu();
			emu->disable_oversampling();
			setup_effects( false );

			try
			{
				m_file->seek( 0, p_abort );
				foobar_Data_Reader rdr( m_file, p_abort );

				emu->set_gain( volume_modifier );
				ERRCHK( emu->set_sample_rate( sample_rate ) );
				ERRCHK( emu->load( rdr ) );
				handle_warning();
			}
			catch(...)
			{
				if ( emu )
				{
					delete emu;
					this->emu = emu = NULL;
				}
				throw;
			}

			m_file.release();
		}

		is_normal_playback = !!( p_flags & input_flag_playback );
		monitor_start();

		emu->start_track( 0 );
		handle_warning();

		played = 0;
		no_infinite = !cfg_indefinite || ( p_flags & input_flag_no_looping );

		if ( no_infinite )
		{
			int song_len = pfc::byteswap_if_be_t( * ( ( t_uint32 * ) &m_header.track_duration ) );
			if ( song_len )
			{
				//int fade_min = ( 512 * 8 * 1000 / 2 + sample_rate - 1 ) / sample_rate;
				int loop_len = pfc::byteswap_if_be_t( * ( ( t_uint32 * ) &m_header.loop_duration ) );
				song_len += loop_len * cfg_vgm_loop_count;
				emu->set_fade( song_len * 10 / 441, tag_fade_ms );
			}
		}

		subsong = 0;

		stop_on_errors = !! ( p_flags & input_flag_testing_integrity );

		first_block = true;
	}
};

namespace c { DECLARE_FILE_TYPE("VGM files", "*.VGM;*.VGZ"); }

static input_factory_t<input_vgm> g_input_vgm_factory;
