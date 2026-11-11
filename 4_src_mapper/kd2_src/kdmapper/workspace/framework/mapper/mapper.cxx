#include <impl/include.hxx>

void mapper::relocate_image_by_delta( portable_executable::vec_relocs relocs, const ULONG64 delta )
{
	for ( const auto& current_reloc : relocs )
	{
		for ( auto i = 0u; i < current_reloc.count; ++i )
		{
			const uint16_t type = current_reloc.item[ i ] >> 12;
			const uint16_t offset = current_reloc.item[ i ] & 0xFFF;

			if ( type == IMAGE_REL_BASED_DIR64 )
				*reinterpret_cast< ULONG64* >( current_reloc.address + offset ) += delta;
		};
	};
};

bool mapper::fix_security_cookie( void* local_image, ULONG64 kernel_image_base )
{
	auto headers = portable_executable::get_nt_headers( local_image );
	if ( !headers )
		return false;

	auto load_config_directory = headers->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG ].VirtualAddress;
	if ( !load_config_directory )
		return true;

	auto load_config_struct = ( PIMAGE_LOAD_CONFIG_DIRECTORY )( ( uintptr_t )local_image + load_config_directory );
	auto stack_cookie = load_config_struct->SecurityCookie;
	if ( !stack_cookie )
		return true;

	stack_cookie = stack_cookie - ( uintptr_t )kernel_image_base + ( uintptr_t )local_image;

	if ( *( uintptr_t* )( stack_cookie ) != 0x2B992DDFA232 )
		return false;

	auto new_cookie = 0x2B992DDFA232 ^ GetCurrentProcessId( ) ^ GetCurrentThreadId( );
	if ( new_cookie == 0x2B992DDFA232 )
		new_cookie = 0x2B992DDFA233;

	*( uintptr_t* )( stack_cookie ) = new_cookie;
	return true;
};

bool mapper::resolve_imports( portable_executable::vec_imports imports )
{
	for ( const auto& current_import : imports )
	{
		ULONG64 module = intel_driver::get_kernel_module_address( current_import.module_name );
		if ( !module )
			return false;

		for ( auto& current_function_data : current_import.function_datas )
		{
			ULONG64 function_address =
				intel_driver::get_kernel_module_export( module, current_function_data.name );

			if ( !function_address )
			{
				if ( module != intel_driver::ntoskrnl_address )
				{
					function_address = intel_driver::get_kernel_module_export(
						intel_driver::ntoskrnl_address,
						current_function_data.name );
					if ( !function_address )
						return false;
				};
			};

			*current_function_data.address = function_address;
		};
	};

	return true;
};

ULONG64 mapper::map_driver(
	BYTE* data,
	ULONG64 param1,
	ULONG64 param2,
	bool free_memory,
	bool destroy_header,
	allocation_mode mode,
	bool pass_allocation_address_as_first_param,
	map_callback callback,
	NTSTATUS* exit_code )
{
	const PIMAGE_NT_HEADERS64 nt_headers = portable_executable::get_nt_headers( data );

	if ( !nt_headers )
	{
		MAPPER_LOG( << skCrypt( "[-] map: invalid PE\n" ) );
		return 0;
	};

	if ( nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC )
	{
		MAPPER_LOG( << skCrypt( "[-] map: need PE32+\n" ) );
		return 0;
	};

	const ULONG32 image_size = nt_headers->OptionalHeader.SizeOfImage;

	void* local_image_base = VirtualAlloc( nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
	if ( !local_image_base )
	{
		MAPPER_LOG( << skCrypt( "[-] map: VirtualAlloc\n" ) );
		return 0;
	};

	const DWORD header_region_size = ( IMAGE_FIRST_SECTION( nt_headers ) )->VirtualAddress;

	ULONG64 kernel_image_base = 0;

	if ( mode == allocation_mode::allocate_independent_pages )
		kernel_image_base = intel_driver::mm_allocate_independent_pages_ex( image_size );
	else
		kernel_image_base = intel_driver::allocate_pool( nt::POOL_TYPE::NonPagedPool, image_size );

	if ( !kernel_image_base )
	{
		MAPPER_LOG( << skCrypt( "[-] map: kernel alloc\n" ) );
		VirtualFree( local_image_base, 0, MEM_RELEASE );
		return 0;
	};

	do
	{
		memcpy( local_image_base, data, nt_headers->OptionalHeader.SizeOfHeaders );

		const PIMAGE_SECTION_HEADER current_image_section = IMAGE_FIRST_SECTION( nt_headers );

		for ( auto i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i )
		{
			if ( ( current_image_section[ i ].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA ) > 0 )
				continue;

			auto local_section = reinterpret_cast< void* >(
				( ULONG64 )local_image_base + current_image_section[ i ].VirtualAddress );

			memcpy(
				local_section,
				reinterpret_cast< void* >(
					( ULONG64 )data + current_image_section[ i ].PointerToRawData ),
				current_image_section[ i ].SizeOfRawData );
		};

		const ULONG64 real_base = kernel_image_base;

		relocate_image_by_delta(
			portable_executable::get_relocs( local_image_base ),
			real_base - nt_headers->OptionalHeader.ImageBase );

		if ( !fix_security_cookie( local_image_base, real_base ) )
		{
			MAPPER_LOG( << skCrypt( "[-] map: stack cookie\n" ) );
			kernel_image_base = real_base;
			break;
		};

		if ( !resolve_imports( portable_executable::get_imports( local_image_base ) ) )
		{
			MAPPER_LOG( << skCrypt( "[-] map: imports\n" ) );
			kernel_image_base = real_base;
			break;
		};

		if ( !intel_driver::write_memory( real_base, local_image_base, image_size ) )
		{
			MAPPER_LOG( << skCrypt( "[-] map: write image\n" ) );
			kernel_image_base = real_base;
			break;
		};

		if ( destroy_header && header_region_size > 0 )
		{
			std::vector< uint8_t > zero_header( header_region_size, 0 );
			if ( !intel_driver::write_memory( real_base, zero_header.data( ), header_region_size ) )
			{
				MAPPER_LOG( << skCrypt( "[-] map: zero headers\n" ) );
				kernel_image_base = real_base;
				break;
			};
		};

		if ( mode == allocation_mode::allocate_independent_pages )
		{
			for ( int i = 0; i < nt_headers->FileHeader.NumberOfSections; i++ )
			{
				auto sec = &IMAGE_FIRST_SECTION( nt_headers )[ i ];
				uintptr_t sec_addr = real_base + sec->VirtualAddress;
				uint32_t sec_size = sec->Misc.VirtualSize;

				if ( sec_size <= 0 )
					continue;

				ULONG prot = PAGE_READONLY;

				if ( sec->Characteristics & IMAGE_SCN_MEM_EXECUTE )
					prot =
						( sec->Characteristics & IMAGE_SCN_MEM_WRITE ) ? PAGE_EXECUTE_READWRITE :
																		 PAGE_EXECUTE_READ;
				else if ( sec->Characteristics & IMAGE_SCN_MEM_WRITE )
					prot = PAGE_READWRITE;
				else if ( sec->Characteristics & IMAGE_SCN_MEM_READ )
					prot = PAGE_READONLY;

				if ( !intel_driver::mm_set_page_protection( sec_addr, sec_size, prot ) )
				{}
			};
		};

		const ULONG64 entry_point = real_base + nt_headers->OptionalHeader.AddressOfEntryPoint;

		if ( callback )
		{
			if ( !callback( &param1, &param2, real_base, image_size ) )
			{
				MAPPER_LOG( << skCrypt( "[-] map: callback\n" ) );
				kernel_image_base = real_base;
				break;
			};
		};

		NTSTATUS status = 0;

		if ( !intel_driver::call_kfc(
				 &status,
				 entry_point,
				 ( pass_allocation_address_as_first_param ? real_base : param1 ),
				 param2 ) )
		{
			MAPPER_LOG( << skCrypt( "[-] map: DriverEntry\n" ) );
			kernel_image_base = real_base;
			break;
		};

		if ( exit_code )
			*exit_code = status;

		const auto& fh = nt_headers->FileHeader;
		const auto& oh = nt_headers->OptionalHeader;

		MAPPER_LOG( << std::hex << std::uppercase
				  << skCrypt( "[+] PE: machine=0x" ) << fh.Machine << skCrypt( " sections=0x" )
				  << static_cast< unsigned >( fh.NumberOfSections ) << skCrypt( " timestamp=0x" ) << fh.TimeDateStamp
				  << skCrypt( " symbols=0x" ) << fh.NumberOfSymbols << skCrypt( " symptr=0x" ) << fh.PointerToSymbolTable
				  << skCrypt( "\n[+] PE: imagebase_pref=0x" ) << static_cast< ULONG64 >( oh.ImageBase )
				  << skCrypt( " mapped_kva=0x" ) << real_base << skCrypt( " sizeofimage=0x" ) << oh.SizeOfImage
				  << skCrypt( " sizeofheaders=0x" ) << oh.SizeOfHeaders << skCrypt( " entry_rva=0x" ) << oh.AddressOfEntryPoint
				  << skCrypt( " section_align=0x" ) << oh.SectionAlignment << skCrypt( " file_align=0x" ) << oh.FileAlignment
				  << skCrypt( "\n[+] PE: subsystem=0x" ) << static_cast< unsigned >( oh.Subsystem ) << skCrypt( " dllchars=0x" )
				  << oh.DllCharacteristics << skCrypt( " checksum=0x" ) << oh.CheckSum << skCrypt( " linker=" )
				  << static_cast< unsigned >( oh.MajorLinkerVersion ) << skCrypt( "." )
				  << static_cast< unsigned >( oh.MinorLinkerVersion ) << skCrypt( " osver=" )
				  << static_cast< unsigned >( oh.MajorOperatingSystemVersion ) << skCrypt( "." )
				  << static_cast< unsigned >( oh.MinorOperatingSystemVersion ) << skCrypt( " imgver=" )
				  << static_cast< unsigned >( oh.MajorImageVersion ) << skCrypt( "." )
				  << static_cast< unsigned >( oh.MinorImageVersion ) << skCrypt( " subsysver=" )
				  << static_cast< unsigned >( oh.MajorSubsystemVersion ) << skCrypt( "." )
				  << static_cast< unsigned >( oh.MinorSubsystemVersion ) << skCrypt( "\n[+] PE: entry_kva=0x" )
				  << entry_point << skCrypt( " status=0x" )
				  << static_cast< ULONG64 >( static_cast< LONG64 >( status ) ) << std::dec << std::nouppercase
				  << skCrypt( "\n" ) );

		const PIMAGE_SECTION_HEADER secs = IMAGE_FIRST_SECTION( nt_headers );
		for ( WORD si = 0; si < fh.NumberOfSections; ++si )
		{
			const IMAGE_SECTION_HEADER& s = secs[ si ];
			char nm[ IMAGE_SIZEOF_SHORT_NAME + 1 ] = {};
			memcpy( nm, s.Name, IMAGE_SIZEOF_SHORT_NAME );
			MAPPER_LOG( << std::hex << std::uppercase << skCrypt( "[+] PE sec[" ) << std::dec << static_cast< int >( si )
					  << std::hex << skCrypt( "] " ) << nm << skCrypt( " va=0x" ) << s.VirtualAddress << skCrypt( " vsz=0x" )
					  << s.Misc.VirtualSize << skCrypt( " ptr_raw=0x" ) << s.PointerToRawData << skCrypt( " rawsz=0x" )
					  << s.SizeOfRawData << skCrypt( " chr=0x" ) << s.Characteristics << std::dec << std::nouppercase
					  << skCrypt( "\n" ) );
		};

		if ( destroy_header && header_region_size > 0 )
			MAPPER_LOG( << std::hex << std::uppercase << skCrypt( "[+] PE headers stripped in pool: kva=0x" ) << real_base
					  << skCrypt( " end=0x" ) << ( real_base + header_region_size ) << skCrypt( " bytes=0x" ) << header_region_size
					  << std::dec << std::nouppercase << skCrypt( "\n" ) );
		else
			MAPPER_LOG( << skCrypt( "[+] PE headers: not stripped in kernel (opts)\n" ) );

		if ( free_memory )
		{
			bool free_status = false;

			if ( mode == allocation_mode::allocate_independent_pages )
				free_status = intel_driver::mm_free_independent_pages( real_base, image_size );
			else
				free_status = intel_driver::free_pool( real_base );

			if ( !free_status )
			{}
		};

		VirtualFree( local_image_base, 0, MEM_RELEASE );
		return real_base;

	} while ( false );

	VirtualFree( local_image_base, 0, MEM_RELEASE );

	MAPPER_LOG( << skCrypt( "[-] map: aborted, free kernel buffer\n" ) );
	bool free_status = false;

	if ( mode == allocation_mode::allocate_independent_pages )
		free_status = intel_driver::mm_free_independent_pages( kernel_image_base, image_size );
	else
		free_status = intel_driver::free_pool( kernel_image_base );

	if ( !free_status )
	{}
	return 0;
};
