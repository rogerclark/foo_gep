#include "config.h"

#include "base.h"
#include "reader.h"

#include "coleco.h"

#include <gme/blargg_endian.h>
#include <gme/Gym_Emu.h>
#include <gme/Sgc_Emu.h>

#undef HEADER_STRING
#define HEADER_STRING(i,n,f) meta_add((i), (n), (f), sizeof(f))

static gme_type_t const gme_type_list_ [] = { gme_ay_type, gme_gbs_type, gme_gym_type, gme_hes_type, gme_kss_type, gme_sap_type, gme_sgc_type };

static unsigned identify_header( void const* header )
{
	switch ( get_be32( header ) )
	{
		case BLARGG_4CHAR('Z','X','A','Y'):  return 1;
		case BLARGG_4CHAR('G','B','S',0x01): return 2;
		case BLARGG_4CHAR('G','Y','M','X'):  return 3;
		case BLARGG_4CHAR('H','E','S','M'):  return 4;
		case BLARGG_4CHAR('K','S','C','C'):
		case BLARGG_4CHAR('K','S','S','X'):  return 5;
		case BLARGG_4CHAR('S','A','P',0x0D): return 6;
		case BLARGG_4CHAR('S','G','C',0x1A): return 7;
	}
	return 0;
}

static struct coleco_setup
{
	coleco_setup()
	{
		for ( unsigned i = 0; i < 0x2000; i++ )
		{
			image [i] *= 820109;
		}
		Sgc_Emu::set_coleco_bios( image );
	}
} setup_stuff;

class input_generic : public input_gep
{
	unsigned type;

public:
	static bool g_is_our_path( const char * p_path, const char * p_extension )
	{
		for ( unsigned i = 0, j = tabsize( gme_type_list_ ); i < j; ++i )
		{
			if ( ! ( cfg_format_enable & ( 1 << ( i + 3 ) ) ) ) continue;
			if ( ! stricmp( p_extension, gme_type_list_[ i ]->extension_ ) )
				return 1;
		}
		return 0;
	}

	void open( service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort )
	{
		if ( p_reason == input_open_info_write ) p_reason = input_open_info_read;

		input_gep::open( p_filehint, p_path, p_reason, p_abort );

		char header [4];
		m_file->read_object_t( header, p_abort );
		type = identify_header( header );
		if ( !type ) throw exception_io_data();

		gme_type_t const gtype = gme_type_list_[ type - 1 ];

		// Disallow type / extension overlap
		if ( stricmp( gtype->extension_, pfc::string_extension( p_path ) ) ) throw exception_io_data();

		m_file->seek( 0, p_abort );

		foobar_Data_Reader rdr( m_file, p_abort );

		if ( p_reason == input_open_info_read )
			emu = gtype->new_info();
		else if ( p_reason == input_open_decode )
			emu = gtype->new_emu();
		if ( !emu ) throw std::bad_alloc();

		if ( p_reason == input_open_decode )
		{
			if ( gtype == gme_gym_type )
			{
				static_cast<Gym_Emu *>( emu )->disable_oversampling();
				setup_effects( false );
			}
			else
				setup_effects();
		}

		ERRCHK( emu->set_sample_rate( sample_rate ) );
		ERRCHK( emu->load( rdr ) );
		handle_warning();

		if ( p_reason == input_open_decode && gtype != gme_gym_type )
			setup_effects_2();

		static_api_ptr_t<hasher_md5> p_hasher;
		hasher_md5_state p_hash_state;
		p_hasher->initialize( p_hash_state );
		emu->hash_( info_hasher_md5( p_hasher, p_hash_state ) );
		m_file_hash = p_hasher->get_result( p_hash_state );

		m_file.release();

		if ( gtype != gme_gym_type )
		{
			pfc::string_replace_extension list( p_path, "m3u" );
			if ( filesystem::g_exists( list, p_abort ) )
			{
				filesystem::g_open( p_filehint, list, filesystem::open_mode_read, p_abort );
				foobar_Data_Reader rdr( p_filehint, p_abort );
				ERRCHK( emu->load_m3u( rdr ) );
				handle_warning();
			}
		}
	}

	unsigned get_subsong_count()
	{
		return emu->track_count();
	}

	t_uint32 get_subsong( unsigned p_subsong )
	{
		return p_subsong;
	}

	void get_info( t_uint32 p_subsong, file_info & p_info, abort_callback & p_abort )
	{
		p_info.info_set("codec", gme_type_list_[ type - 1 ]->extension_ );
		p_info.info_set( "encoding", "synthesized" );

		//p_info.info_set_int("samplerate", sample_rate);
		p_info.info_set_int("channels", 2);
		p_info.info_set_int("bitspersample", 16);

		track_info_t i;
		ERRCHK( emu->track_info( &i, p_subsong ) );

		int length = 0;
		double dlength = double(tag_song_ms + tag_fade_ms) * .001;
		if ( i.intro_length != -1 && i.loop_length != -1 )
			length = i.intro_length + i.loop_length * ( i.repeat_count > 0 ? i.repeat_count : 2 );
		else if ( i.length != -1 )
			length = i.length;
		if ( length )
		{
			tag_song_ms = length;
			tag_fade_ms = i.fade_length;
			dlength = double(tag_song_ms + tag_fade_ms) * .001;
		}

		HEADER_STRING( p_info, "system", i.system );
		HEADER_STRING( p_info, "album", i.game );
		HEADER_STRING( p_info, "title", i.song );
		HEADER_STRING( p_info, "artist", i.author );
		HEADER_STRING( p_info, "date", i.date );
		HEADER_STRING( p_info, "engineer", i.engineer );
		HEADER_STRING( p_info, "composer", i.composer );
		HEADER_STRING( p_info, "sequencer", i.sequencer );
		HEADER_STRING( p_info, "copyright", i.copyright );
		HEADER_STRING( p_info, "comment", i.comment );
		HEADER_STRING( p_info, "dumper", i.dumper );
		HEADER_STRING( p_info, "tagger", i.tagger );

		p_info.set_length(dlength);

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
		track_info_t i;
		ERRCHK( emu->track_info( &i, p_subsong ) );

		int length = 0;
		double dlength = double(tag_song_ms + tag_fade_ms) * .001;
		if ( i.intro_length != -1 && i.loop_length != -1 )
			length = i.intro_length + i.loop_length * ( i.repeat_count > 0 ? i.repeat_count : 2 );
		else if ( i.length != -1 )
			length = i.length;
		if ( length )
		{
			tag_song_ms = length;
			tag_fade_ms = i.fade_length;
			dlength = double(tag_song_ms + tag_fade_ms) * .001;
		}

		input_gep::decode_initialize( p_subsong, p_flags, p_abort );
	}
};

#define DECLARE_FILE_TYPE_n(NS,NAME,MASK) \
	namespace NS { static input_file_type_impl g_filetype_instance(NAME,MASK,true); \
	static service_factory_single_ref_t<input_file_type_impl> g_filetype_service(g_filetype_instance); }

DECLARE_FILE_TYPE_n( a, "AY files", "*.AY" );
DECLARE_FILE_TYPE_n( b, "GBS files", "*.GBS" );
DECLARE_FILE_TYPE_n( c, "GYM files", "*.GYM" );
DECLARE_FILE_TYPE_n( d, "HES files", "*.HES" );
DECLARE_FILE_TYPE_n( e, "KSS files", "*.KSS" );
DECLARE_FILE_TYPE_n( f, "SAP files", "*.SAP" );
DECLARE_FILE_TYPE_n( g, "SGC files", "*.SGC" );

static input_factory_t        <input_generic>   g_input_factory_generic;
