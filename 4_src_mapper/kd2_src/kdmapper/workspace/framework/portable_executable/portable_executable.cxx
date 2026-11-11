#include <framework/portable_executable/portable_executable.hxx>

PIMAGE_NT_HEADERS64 portable_executable::get_nt_headers( void* image_base )
{
	const auto dos_header = reinterpret_cast< PIMAGE_DOS_HEADER >( image_base );
	if ( dos_header->e_magic != IMAGE_DOS_SIGNATURE )
		return nullptr;

	const auto nt_headers = reinterpret_cast< PIMAGE_NT_HEADERS64 >(
		reinterpret_cast< ULONG64 >( image_base ) + dos_header->e_lfanew );
	if ( nt_headers->Signature != IMAGE_NT_SIGNATURE )
		return nullptr;

	return nt_headers;
};

portable_executable::vec_relocs portable_executable::get_relocs( void* image_base )
{
	const PIMAGE_NT_HEADERS64 nt_headers = get_nt_headers( image_base );
	if ( !nt_headers )
		return {};

	vec_relocs relocs;
	DWORD reloc_va = nt_headers->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_BASERELOC ].VirtualAddress;
	if ( !reloc_va )
		return {};

	auto cb_relocation = reinterpret_cast< PIMAGE_BASE_RELOCATION >(
		reinterpret_cast< ULONG64 >( image_base ) + reloc_va );
	const auto reloc_end = reinterpret_cast< PIMAGE_BASE_RELOCATION >(
		reinterpret_cast< ULONG64 >( cb_relocation )
		+ nt_headers->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_BASERELOC ].Size );

	while ( cb_relocation < reloc_end && cb_relocation->SizeOfBlock )
	{
		reloc_info r;
		r.address = reinterpret_cast< ULONG64 >( image_base ) + cb_relocation->VirtualAddress;
		r.item = reinterpret_cast< USHORT* >(
			reinterpret_cast< ULONG64 >( cb_relocation ) + sizeof( IMAGE_BASE_RELOCATION ) );
		r.count = ( cb_relocation->SizeOfBlock - sizeof( IMAGE_BASE_RELOCATION ) ) / sizeof( USHORT );
		relocs.push_back( r );
		cb_relocation = reinterpret_cast< PIMAGE_BASE_RELOCATION >(
			reinterpret_cast< ULONG64 >( cb_relocation ) + cb_relocation->SizeOfBlock );
	};

	return relocs;
};

portable_executable::vec_imports portable_executable::get_imports( void* image_base )
{
	const PIMAGE_NT_HEADERS64 nt_headers = get_nt_headers( image_base );
	if ( !nt_headers )
		return {};

	DWORD import_va = nt_headers->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_IMPORT ].VirtualAddress;
	if ( !import_va )
		return {};

	vec_imports imports;

	auto current_import_descriptor = reinterpret_cast< PIMAGE_IMPORT_DESCRIPTOR >(
		reinterpret_cast< ULONG64 >( image_base ) + import_va );

	while ( current_import_descriptor->FirstThunk )
	{
		import_info ii;
		ii.module_name = std::string( reinterpret_cast< char* >(
			reinterpret_cast< ULONG64 >( image_base ) + current_import_descriptor->Name ) );

		auto current_first_thunk = reinterpret_cast< PIMAGE_THUNK_DATA64 >(
			reinterpret_cast< ULONG64 >( image_base ) + current_import_descriptor->FirstThunk );
		auto current_original_first_thunk = reinterpret_cast< PIMAGE_THUNK_DATA64 >(
			reinterpret_cast< ULONG64 >( image_base ) + current_import_descriptor->OriginalFirstThunk );

		while ( current_original_first_thunk->u1.Function )
		{
			import_function_info import_function_data;
			auto thunk_data = reinterpret_cast< PIMAGE_IMPORT_BY_NAME >(
				reinterpret_cast< ULONG64 >( image_base ) + current_original_first_thunk->u1.AddressOfData );

			import_function_data.name = thunk_data->Name;
			import_function_data.address = &current_first_thunk->u1.Function;
			ii.function_datas.push_back( import_function_data );

			++current_original_first_thunk;
			++current_first_thunk;
		};

		imports.push_back( ii );
		++current_import_descriptor;
	};

	return imports;
};
