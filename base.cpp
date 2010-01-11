#include "base.h"
#include "config.h"

input_gep::input_gep()
{
	emu = 0;
	voice_mask = 0;

	sample_rate = cfg_sample_rate;
}

input_gep::~input_gep()
{
	if (emu)
	{
		if ( emu->error_count() )
			console::formatter() << "Emulation errors: " << emu->error_count();
		delete emu;
	}
}

void input_gep::open( service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort )
{
	if ( p_filehint.is_empty() )
	{
		filesystem::g_open( m_file, p_path, ( p_reason == input_open_info_write ) ? filesystem::open_mode_write_existing : filesystem::open_mode_read, p_abort );
	}
	else m_file = p_filehint;

	m_stats = m_file->get_stats( p_abort );

	m_path = p_path;

	tag_song_ms = cfg_default_length;
	tag_fade_ms = cfg_default_fade;
}

unsigned input_gep::get_subsong_count()
{
	return 1;
}

t_uint32 input_gep::get_subsong( unsigned p_index )
{
	return p_index;
}

t_filestats input_gep::get_file_stats( abort_callback & p_abort )
{
	return m_stats;
}

void input_gep::decode_initialize( t_uint32 p_subsong, unsigned p_flags, abort_callback & p_abort )
{
	played = 0;
	no_infinite = !cfg_indefinite || ( p_flags & input_flag_no_looping );

	song_len=MulDiv(tag_song_ms, sample_rate, 1000);
	fade_len=MulDiv(tag_fade_ms, sample_rate, 1000);

	subsong = p_subsong;
	emu->start_track( subsong );

	stop_on_errors = !! ( p_flags & input_flag_testing_integrity );
}

bool input_gep::decode_run( audio_chunk & p_chunk,abort_callback & p_abort )
{
	if ( ! emu->track_ended() )
	{
		if ( no_infinite && played >= song_len + fade_len ) return false;

		sample_buffer.grow_size( 1024 );
		blip_sample_t * buf = sample_buffer.get_ptr();
		emu->play( 1024, buf );

		if ( stop_on_errors && emu->error_count() ) throw exception_io_data();

		int d_start,d_end;
		d_start = played;
		played += 512;
		d_end   = played;

		if ( no_infinite && song_len && d_end > song_len )
		{
			blip_sample_t * foo = buf;
			for(int n = d_start; n < d_end; n++)
			{
				if ( n > song_len )
				{
					if ( n <= song_len + fade_len )
					{
						int bleh = song_len + fade_len - n;
						*foo++ = MulDiv( *foo, bleh, fade_len );
						*foo++ = MulDiv( *foo, bleh, fade_len );
					}
					else
					{
						*foo++ = 0;
						*foo++ = 0;
					}
				}
			}
		}

		p_chunk.set_data_fixedpoint( buf, 1024 * sizeof( blip_sample_t ), sample_rate, 2, sizeof( blip_sample_t ) * 8, audio_chunk::channel_config_stereo );

		return true;
	}

	return false;
}

void input_gep::decode_seek( double p_seconds, abort_callback & p_abort )
{
	int now = int( audio_math::time_to_samples( p_seconds, sample_rate ) );
	if ( now == played ) return;
	else if ( now < played )
	{
		emu->start_track( subsong );
		played = 0;
	}

	sample_buffer.grow_size( 1024 );
	blip_sample_t * buf = sample_buffer.get_ptr();
	int played_ = played;

	if ( now - played_ > int( sample_rate ) * 4 )
	{
		now -= sample_rate * 4;

		emu->mute_voices( ~0 );

		while ( played_ < now )
		{
			p_abort.check();

			long todo = now - played_;
			if ( todo > 512 ) todo = 512;
			emu->play( todo * 2, buf );
			played_ += todo;
		}

		now += sample_rate * 4;
	}

	emu->mute_voices( voice_mask );

	while ( played_ < now )
	{
		p_abort.check();

		long todo = now - played_;
		if ( todo > 512 ) todo = 512;
		emu->play( todo * 2, buf );
		played_ += todo;
	}

	played = played_;
}

bool input_gep::decode_can_seek()
{
	return true;
}

bool input_gep::decode_get_dynamic_info( file_info & p_out, double & p_timestamp_delta )
{
	return false;
}

bool input_gep::decode_get_dynamic_info_track( file_info & p_out, double & p_timestamp_delta )
{
	return false;
}

void input_gep::decode_on_idle( abort_callback & p_abort )
{
}

void input_gep::retag_set_info( t_uint32 p_subsong, const file_info & p_info, abort_callback & p_abort )
{
	throw exception_io_data();
}

void input_gep::retag_commit( abort_callback & p_abort )
{
	throw exception_io_data();
}

bool input_gep::g_is_our_content_type( const char * p_content_type )
{
	return false;
}