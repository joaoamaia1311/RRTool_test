#pragma once
#include <string>
#include <vector>
#include <windows.h>

#include <protection/sk_crypt.hxx>

namespace util
{
	void log( const char* message );
	void log( const std::string& message );
	void log_error( const char* message );
	void log_error( const std::string& message );

	bool read_file_to_memory( const std::wstring& file_path, std::vector< BYTE >* out_buffer );
	bool create_file_from_memory( const std::wstring& desired_file_path, const char* address, size_t size );
	const char* ntstatus_to_string( NTSTATUS status );
};
