#include <intel/service/service.hxx>

NTSTATUS service::start( const std::wstring& driver_path, const std::wstring& service_name )
{
	const static DWORD service_type_kernel = 1;

	const std::wstring services_path = std::wstring( skCrypt( L"SYSTEM\\CurrentControlSet\\Services\\" ) ) + service_name;
	const std::wstring n_path = std::wstring( skCrypt( L"\\??\\" ) ) + driver_path;

	HKEY d_service;
	LSTATUS status = RegCreateKeyW( HKEY_LOCAL_MACHINE, services_path.c_str( ), &d_service );

	if ( status != ERROR_SUCCESS )
		return STATUS_REGISTRY_IO_FAILED;

	status = RegSetKeyValueW(
		d_service,
		NULL,
		skCrypt( L"ImagePath" ),
		REG_EXPAND_SZ,
		n_path.c_str( ),
		( DWORD )( n_path.size( ) * sizeof( wchar_t ) ) );

	if ( status != ERROR_SUCCESS )
	{
		RegCloseKey( d_service );
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
		return STATUS_REGISTRY_IO_FAILED;
	};

	status = RegSetKeyValueW(
		d_service,
		NULL,
		skCrypt( L"Type" ),
		REG_DWORD,
		&service_type_kernel,
		sizeof( DWORD ) );

	if ( status != ERROR_SUCCESS )
	{
		RegCloseKey( d_service );
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
		return STATUS_REGISTRY_IO_FAILED;
	};

	RegCloseKey( d_service );

	HMODULE ntdll = GetModuleHandleA( skCrypt( "ntdll.dll" ) );
	if ( ntdll == NULL )
	{
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
		return STATUS_UNSUCCESSFUL;
	};

	ULONG se_load_driver_privilege = 10UL;
	BOOLEAN se_load_driver_was_enabled;

	NTSTATUS nt_status = nt::RtlAdjustPrivilege(
		se_load_driver_privilege,
		TRUE,
		FALSE,
		&se_load_driver_was_enabled );

	if ( !NT_SUCCESS( nt_status ) )
	{
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
		return nt_status;
	};

	std::wstring wdriver_reg_path =
		std::wstring( skCrypt( L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\" ) ) + service_name;

	UNICODE_STRING service_str;
	RtlInitUnicodeString( &service_str, wdriver_reg_path.c_str( ) );

	nt_status = nt::NtLoadDriver( &service_str );

	if ( !NT_SUCCESS( nt_status ) )
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );

	return nt_status;
};

NTSTATUS service::stop( const std::wstring& service_name )
{
	HMODULE ntdll = GetModuleHandleA( skCrypt( "ntdll.dll" ) );
	if ( ntdll == NULL )
		return STATUS_UNSUCCESSFUL;

	const ULONG se_load_driver_privilege = 10UL;
	BOOLEAN se_load_driver_was_enabled = FALSE;
	NTSTATUS priv_status = nt::RtlAdjustPrivilege(
		se_load_driver_privilege,
		TRUE,
		FALSE,
		&se_load_driver_was_enabled );
	if ( !NT_SUCCESS( priv_status ) )
		return priv_status;

	std::wstring wdriver_reg_path =
		std::wstring( skCrypt( L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\" ) ) + service_name;
	UNICODE_STRING service_string;
	RtlInitUnicodeString( &service_string, wdriver_reg_path.c_str( ) );

	HKEY d_service;
	std::wstring services_path = std::wstring( skCrypt( L"SYSTEM\\CurrentControlSet\\Services\\" ) ) + service_name;
	LSTATUS status = RegOpenKeyW( HKEY_LOCAL_MACHINE, services_path.c_str(), &d_service );

	if ( status != ERROR_SUCCESS )
	{
		if ( status == ERROR_FILE_NOT_FOUND )
			return STATUS_SUCCESS;
		return STATUS_REGISTRY_IO_FAILED;
	};

	RegCloseKey( d_service );

	NTSTATUS st = nt::NtUnloadDriver( &service_string );
	if ( !NT_SUCCESS( st ) )
	{
		RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
		return st;
	};

	status = RegDeleteTreeW( HKEY_LOCAL_MACHINE, services_path.c_str( ) );
	if ( status != ERROR_SUCCESS )
		return STATUS_REGISTRY_IO_FAILED;

	return st;
};