#include <foobar2000.h>

#include <gme/Music_Emu.h>
#include <gme/Multi_Buffer.h>
#include <gme/Effects_Buffer.h>

#define ERRCHK(f) \
	{ \
		blargg_err_t err = (f); \
		if (err) \
		{ \
			console::info(err); \
			throw exception_io_data(err); \
		} \
	}

#define HEADER_STRING(i,n,f) if ((f)[0]) (i).meta_add((n), pfc::stringcvt::string_utf8_from_ansi((f), sizeof((f))))

class info_hasher_md5 : public Music_Emu::Hash_Function
{
	static_api_ptr_t<hasher_md5> & m_hasher;
	hasher_md5_state & m_state;

public:
	info_hasher_md5( static_api_ptr_t<hasher_md5> & p_hasher, hasher_md5_state & p_state ) : m_hasher( p_hasher ), m_state( p_state ) { }
	void hash_( byte const* data, size_t size ) { m_hasher->process( m_state, data, size ); }
};

extern const GUID guid_gep_index;

class metadb_index_client_gep : public metadb_index_client
{
	virtual metadb_index_hash transform(const file_info & info, const playable_location & location);
};

class input_gep
{
protected:

	gme_t                 * emu;

	Simple_Effects_Buffer     * buffer;

	void                      * resampler[2];

	unsigned                    sample_rate;

	double                      sample_rate_scale;
	bool                        last_block_was_resampled;

	int                         subsong;
	//int                         voice_mask;

	int                         played;

	int                         tag_song_ms;
	int                         tag_fade_ms;
	/*int                         song_len;
	int                         fade_len;*/
	bool                        no_infinite;

	bool                        stop_on_errors;

	bool                        first_block;

	bool                        monitoring;
	bool                        is_normal_playback;

	bool                        effects_enable;
	int                         effects_bass;
	int                         effects_treble;
	int                         effects_echo_depth;

	pfc::array_t<blip_sample_t> sample_buffer;
	pfc::array_t<blip_sample_t> sample_buffer_resampled;

	service_ptr_t<file>         m_file;
	pfc::string_simple          m_path;
	t_filestats                 m_stats;

	metadb_index_hash           m_index_hash;
	hasher_md5_result           m_file_hash;

	void handle_warning(gme_t * emu = NULL);

	void meta_add( file_info & p_info, const char * name, const char * value, t_size max );

	void monitor_start();
	void monitor_update();
	void monitor_stop();

	void setup_effects( bool echo = true );
	void setup_effects_2();

public:
	input_gep();
	~input_gep();

	void open( service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort );

	unsigned get_subsong_count();
	t_uint32 get_subsong( unsigned p_index );

	//void get_info( t_uint32 p_subsong, file_info & p_info, abort_callback & p_abort );

	t_filestats get_file_stats( abort_callback & p_abort );

	void decode_initialize( t_uint32 p_subsong, unsigned p_flags, abort_callback & p_abort );
	bool decode_run( audio_chunk & p_chunk, abort_callback & p_abort );
	void decode_seek( double p_seconds, abort_callback & p_abort );
	bool decode_can_seek();
	bool decode_get_dynamic_info( file_info & p_out, double & p_timestamp_delta );
	bool decode_get_dynamic_info_track( file_info & p_out, double & p_timestamp_delta );
	void decode_on_idle( abort_callback & p_abort );

	void retag_set_info( t_uint32 p_subsong, const file_info & p_info, abort_callback & p_abort );
	void retag_commit( abort_callback & p_abort );

	static bool g_is_our_content_type( const char * p_content_type );
};