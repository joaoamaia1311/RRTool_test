#include <impl/include.hxx>
#include <channel/channel_protocol.hxx>

#include <cstring>
#include <fstream>
#include <sstream>

// Mapper entry point. Legacy channel-handshake blocks are inactive.

bool callback( ULONG64* param_one, ULONG64* param_two, ULONG64 allocation_pointer, ULONG64 allocation_size )
{
	UNREFERENCED_PARAMETER( param_two );
	UNREFERENCED_PARAMETER( allocation_size );
	/* Legacy channel-handshake update is inactive. */
	return true;
};

std::wstring get_exe_directory( void );

std::wstring get_channel_directory( void )
{
	wchar_t temp_path[ MAX_PATH ]{};
	const DWORD length = GetTempPathW( MAX_PATH, temp_path );
	if ( !length || length >= MAX_PATH )
		return get_exe_directory( );
	std::wstring directory( temp_path );
	if ( !directory.empty( ) && directory.back( ) != L'\\' && directory.back( ) != L'/' )
		directory += L'\\';
	directory += skCrypt( L"zxmapper" );
	std::filesystem::create_directories( directory );
	return directory;
};

bool write_channel_session( const channel::map_handshake& handshake )
{
	/* Legacy channel session-file serialization is inactive. */
};

std::wstring get_exe_directory( void )
{
	wchar_t path[ MAX_PATH ];
	GetModuleFileNameW( NULL, path, MAX_PATH );
	return std::filesystem::path( path ).parent_path( );
};

int main( int argc, char* argv[] )
{
	// Accept --auto at any argument position so scripted launches do not pause.
	// The positional driver argument is separate from this flag check.
	// Without --auto, the console may wait for user input.
	bool auto_mode = false;
	for ( int i = 1; i < argc; ++i )
		if ( argv[ i ] && std::strcmp( argv[ i ], skCrypt( "--auto" ) ) == 0 )
			auto_mode = true;
	if ( !NT_SUCCESS( intel_driver::load( ) ) )
	{
		util::log_error( skCrypt( "Vulnerable driver load failed" ) );
		if ( !auto_mode )
			std::cin.get( );
		return 1;
	};
	util::log( skCrypt( "Vulnerable driver loaded" ) );
	intel_driver::cache_trace_cleanup_context( );
	util::log( skCrypt( "Cleaning driver traces" ) );
	// The CI hook call is disabled in this build.
	// The earlier implementation patched ci.dll code and was associated with
	// CRITICAL_STRUCTURE_CORRUPTION crashes on the tested Windows build.
	intel_driver::perform_load_trace_cleanup( false );
	std::wstring driver_path;
	std::wstring local_path = get_exe_directory( ) + std::wstring( skCrypt( L"\\kernel-mode.sys" ) );
	if ( std::filesystem::exists( local_path ) )
		driver_path = local_path;
	else
	{
		util::log_error( skCrypt( "kernel-mode.sys is missing" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	std::vector< uint8_t > raw_image;
	if ( !util::read_file_to_memory( driver_path, &raw_image ) )
	{
		util::log_error( skCrypt( "Failed to read driver image" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	mapper::allocation_mode mode = mapper::allocation_mode::allocate_pool;
	std::uint32_t client_pid = GetCurrentProcessId( );
	/* Legacy channel target-file and handshake setup is inactive. */
	util::log( skCrypt( "Mapping kernel driver" ) );
	NTSTATUS exit_code = 0;
	const ULONG64 mapped_base = mapper::map_driver(
		raw_image.data( ),
		0, // No channel-handshake pointer is passed.
		0,
		false,
		true,
		mode,
		false,
		callback,
		&exit_code );
	if ( !mapped_base )
	{
		util::log_error( skCrypt( "map_driver failed" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	{
		std::ostringstream stream;
		stream << skCrypt( "DriverEntry " ) << util::ntstatus_to_string( exit_code ) << skCrypt( " (0x" ) << std::hex << exit_code << std::dec << ')';
		util::log( stream.str( ) );
    };
	/* Legacy channel session logging is inactive. */
	util::log( skCrypt( "Cleaning driver traces" ) );
	intel_driver::cache_trace_cleanup_context( );
	intel_driver::perform_load_trace_cleanup( false );
	const NTSTATUS unload_status = intel_driver::unload( );
	if ( !NT_SUCCESS( unload_status ) )
	{
		std::ostringstream stream;
		stream << skCrypt( "Vulnerable driver unload failed (0x" ) << std::hex << unload_status << std::dec << ')';
		util::log_error( stream.str( ) );
	}
	else
		util::log( skCrypt( "Vulnerable driver unloaded" ) );
	if ( intel_driver::is_running( ) )
		util::log_error( skCrypt( "Vulnerable driver is still active, restart or unload manually" ) );
	if ( !auto_mode )
		std::cin.get( );
	return 0;
};