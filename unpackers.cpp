#include <foobar2000.h>

#include <fex/Zip7_Extractor.h>
#include <fex/Rar_Extractor.h>

#include <memory>

#include "reader.h"

static void handle_error( const char * str )
{
	if ( str ) throw exception_io_data( str );
}

static t_filestats get_stats_in_archive( const char * p_archive, const char * p_file, abort_callback & p_abort )
{
	service_ptr_t< file > m_file;
	filesystem::g_open( m_file, p_archive, filesystem::open_mode_read, p_abort );
	foobar_File_Reader in( m_file, p_abort );
	std::unique_ptr<fex_t> ex( new Zip7_Extractor );
	if ( ex->open( &in ) )
	{
		ex.reset( new Rar_Extractor );
		handle_error( in.seek( 0 ) );
	}
	handle_error( ex->open( &in ) );
	while ( ! ex->done() )
	{
		handle_error( ex->stat() );
		if ( ! strcmp( ex->name(), p_file ) ) break;
		handle_error( ex->next() );
	}
	if ( ex->done() ) throw exception_io_not_found();
	t_filestats ret;
	ret.m_size = ex->size();
	ret.m_timestamp = m_file->get_timestamp( p_abort );
	return ret;
}

static void open_archive( service_ptr_t< file > & p_out, const char * p_archive, const char * p_file, abort_callback & p_abort )
{
	service_ptr_t< file > m_file;
	filesystem::g_open( m_file, p_archive, filesystem::open_mode_read, p_abort );
	t_filetimestamp timestamp = m_file->get_timestamp( p_abort );
	foobar_File_Reader in( m_file, p_abort );
	std::unique_ptr<fex_t> ex( new Zip7_Extractor );
	if ( ex->open( &in ) )
	{
		ex.reset( new Rar_Extractor );
		in.seek( 0 );
	}
	handle_error( ex->open( &in ) );
	while ( ! ex->done() )
	{
		handle_error( ex->stat() );
		if ( ! strcmp( ex->name(), p_file ) ) break;
		handle_error( ex->next() );
	}
	if ( ex->done() ) throw exception_io_not_found();
	const void * data;
	handle_error( ex->data( &data ) );
	p_out = new service_impl_t<reader_membuffer_simple>( data, (t_size)ex->size(), timestamp, m_file->is_remote() );
}

static void archive_list( archive * owner, const char * type, const char * path, const service_ptr_t< file > & p_reader, archive_callback & p_out, bool p_want_readers )
{
	if ( stricmp_utf8( pfc::string_extension( path ), type ) )
		throw exception_io_data();

	service_ptr_t< file > m_file = p_reader;
	if ( m_file.is_empty() )
		filesystem::g_open( m_file, path, filesystem::open_mode_read, p_out );
	else
		m_file->reopen( p_out );

	t_filetimestamp timestamp = m_file->get_timestamp( p_out );

	foobar_File_Reader in( m_file, p_out );
	std::unique_ptr<fex_t> ex( new Zip7_Extractor );
	if ( ex->open( &in ) )
	{
		ex.reset( new Rar_Extractor );
		in.seek( 0 );
	}
	handle_error( ex->open( &in ) );
	pfc::string8_fastalloc m_path;
	service_ptr_t<file> m_out_file;
	t_filestats m_stats;
	m_stats.m_timestamp = timestamp;
	while ( ! ex->done() )
	{
		handle_error( ex->stat() );
		archive_impl::g_make_unpack_path( m_path, path, ex->name(), type );
		m_stats.m_size = ex->size();
		if ( p_want_readers )
		{
			const void * data;
			handle_error( ex->data( &data ) );
			m_out_file = new service_impl_t<reader_membuffer_simple>( data, (t_size)m_stats.m_size, timestamp, m_file->is_remote() );
		}
		if ( ! p_out.on_entry( owner, m_path, m_stats, m_out_file ) ) break;
		handle_error( ex->next() );
	}
}

class archive_rsn : public archive_impl
{
public:
	virtual bool supports_content_types()
	{
		return false;
	}

	virtual const char * get_archive_type()
	{
		return "rsn";
	}

	virtual t_filestats get_stats_in_archive( const char * p_archive, const char * p_file, abort_callback & p_abort )
	{
		return ::get_stats_in_archive( p_archive, p_file, p_abort );
	}

	virtual void open_archive( service_ptr_t< file > & p_out, const char * p_archive, const char * p_file, abort_callback & p_abort )
	{
		::open_archive( p_out, p_archive, p_file, p_abort );
	}

	virtual void archive_list( const char * path, const service_ptr_t< file > & p_reader, archive_callback & p_out, bool p_want_readers )
	{
		::archive_list( this, get_archive_type(), path, p_reader, p_out, p_want_readers );
	}
};

class archive_vgm7z : public archive_impl
{
public:
	virtual bool supports_content_types()
	{
		return false;
	}

	virtual const char * get_archive_type()
	{
		return "vgm7z";
	}

	virtual t_filestats get_stats_in_archive( const char * p_archive, const char * p_file, abort_callback & p_abort )
	{
		return ::get_stats_in_archive( p_archive, p_file, p_abort );
	}

	virtual void open_archive( service_ptr_t< file > & p_out, const char * p_archive, const char * p_file, abort_callback & p_abort )
	{
		::open_archive( p_out, p_archive, p_file, p_abort );
	}

	virtual void archive_list( const char * path, const service_ptr_t< file > & p_reader, archive_callback & p_out, bool p_want_readers )
	{
		::archive_list( this, get_archive_type(), path, p_reader, p_out, p_want_readers );
	}
};

class unpacker_stuff : public unpacker
{
	inline bool skip_ext( const char * p )
	{
		static const char * exts[] = { "txt", "nfo", "info", "diz" };
		pfc::string_extension ext( p );
		for ( unsigned n = 0; n < tabsize( exts ); ++n )
		{
			if ( ! stricmp_utf8( ext, exts[ n ] ) ) return true;
		}
		return false;
	}

public:
	virtual void open( service_ptr_t< file > & p_out, const service_ptr_t< file > & p_source, abort_callback & p_abort )
	{
		if ( p_source.is_empty() ) throw exception_io_data();

		foobar_File_Reader in( p_source, p_abort );
		std::unique_ptr<fex_t> ex( new Zip7_Extractor );
		if ( ex->open( &in ) )
		{
			ex.reset( new Rar_Extractor );
			in.seek( 0 );
		}
		handle_error( ex->open( &in ) );
		while ( ! ex->done() )
		{
			handle_error( ex->stat() );
			if ( ! skip_ext( ex->name() ) )
			{
				const void * data;
				handle_error( ex->data( &data ) );
				p_out = new service_impl_t<reader_membuffer_simple>( data, (t_size)ex->size(), p_source->get_timestamp( p_abort ), p_source->is_remote() );
				return;
			}
			handle_error( ex->next() );
		}
		throw exception_io_data();
	}
};

static archive_factory_t < archive_rsn >  g_archive_rsn_factory;
static archive_factory_t < archive_vgm7z > g_archive_vgm7z_factory;
static unpacker_factory_t< unpacker_stuff > g_unpacker_7z_factory;

namespace b { DECLARE_FILE_TYPE("RSN files", "*.RSN"); }
namespace d { DECLARE_FILE_TYPE("VGM7Z files", "*.VGM7Z"); }
