#pragma once
#include <string>
#include <windows.h>

#include <protection/sk_crypt.hxx>

#include <nt/nt.hxx>

namespace service
{
	NTSTATUS start( const std::wstring& driver_path, const std::wstring& service_name );
	NTSTATUS stop( const std::wstring& service_name );
};
