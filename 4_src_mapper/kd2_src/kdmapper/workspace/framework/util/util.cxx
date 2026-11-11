#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>

#include <impl/include.hxx>

namespace
{
	constexpr WORD color_blue = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	constexpr WORD color_white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	constexpr WORD color_red = FOREGROUND_RED | FOREGROUND_INTENSITY;

	void set_console_color( WORD color )
	{
		SetConsoleTextAttribute( GetStdHandle( STD_OUTPUT_HANDLE ), color );
	};

	std::string current_time_string( )
	{
		const auto now = std::chrono::system_clock::now( );
		const std::time_t time = std::chrono::system_clock::to_time_t( now );
		std::tm local_time{};
		localtime_s( &local_time, &time );

		std::ostringstream stream;
		stream << std::setfill( '0' ) << std::setw( 2 ) << local_time.tm_hour << ':'
			<< std::setw( 2 ) << local_time.tm_min << ':' << std::setw( 2 ) << local_time.tm_sec;
		return stream.str( );
	};

	void write_log_line( const char* message, WORD message_color )
	{
		if ( !mapper::logs_enabled )
			return;

		set_console_color( color_blue );
		std::cout << "[Kd Mapper][" << current_time_string( ) << ']';
		set_console_color( message_color );
		std::cout << " ( " << message << " )" << std::endl;
		set_console_color( color_white );
	};
}

void util::log( const char* message )
{
	write_log_line( message, color_white );
};

void util::log( const std::string& message )
{
	write_log_line( message.c_str( ), color_white );
};

void util::log_error( const char* message )
{
	write_log_line( message, color_red );
};

void util::log_error( const std::string& message )
{
	write_log_line( message.c_str( ), color_red );
};

bool util::read_file_to_memory( const std::wstring& file_path, std::vector< BYTE >* out_buffer )
{
	std::ifstream file_ifstream( file_path, std::ios::binary );

	if ( !file_ifstream )
		return false;

	out_buffer->assign( ( std::istreambuf_iterator< char >( file_ifstream ) ), std::istreambuf_iterator< char >( ) );
	file_ifstream.close( );

	return true;
};

bool util::create_file_from_memory( const std::wstring& desired_file_path, const char* address, size_t size )
{
	if ( !address || !size || desired_file_path.empty( ) )
		return false;

	const HANDLE file_handle = CreateFileW(
		desired_file_path.c_str( ),
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr );
	if ( file_handle == INVALID_HANDLE_VALUE )
		return false;

	DWORD bytes_written = 0;
	const BOOL write_ok = WriteFile( file_handle, address, static_cast< DWORD >( size ), &bytes_written, nullptr );
	CloseHandle( file_handle );

	return write_ok && bytes_written == size;
};

const char* util::ntstatus_to_string( NTSTATUS status )
{
	static char buffer[ 512 ];

	DWORD msg = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
		GetModuleHandleA( skCrypt( "ntdll.dll" ) ),
		status,
		0,
		buffer,
		sizeof( buffer ),
		NULL );

	if ( msg )
		return buffer;

	switch ( status )
	{
	case 0x00000000:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_SUCCESS" ) );
		return buffer;
	case 0xC0000001:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_UNSUCCESSFUL" ) );
		return buffer;
	case 0xC0000005:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_ACCESS_VIOLATION" ) );
		return buffer;
	case 0xC000000D:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_INVALID_PARAMETER" ) );
		return buffer;
	case 0xC0000008:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_INVALID_HANDLE" ) );
		return buffer;
	case 0xC0000010:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_INVALID_DEVICE_REQUEST" ) );
		return buffer;
	case 0xC0000022:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_ACCESS_DENIED" ) );
		return buffer;
	case 0xC0000034:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_OBJECT_NAME_NOT_FOUND" ) );
		return buffer;
	case 0xC0000035:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_OBJECT_NAME_COLLISION" ) );
		return buffer;
	case 0xC0000225:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_NOT_FOUND" ) );
		return buffer;
	case 0xC0000428:
		strcpy_s( buffer, sizeof( buffer ), skCrypt( "STATUS_INVALID_IMAGE_HASH" ) );
		return buffer;
	default:
		break;
	};

	sprintf_s( buffer, sizeof( buffer ), skCrypt( "UNKNOWN_NTSTATUS (0x%08X)" ), status );
	return buffer;
};
