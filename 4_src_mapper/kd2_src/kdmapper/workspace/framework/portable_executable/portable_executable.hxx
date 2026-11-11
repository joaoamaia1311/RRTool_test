#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace portable_executable
{
	struct reloc_info
	{
		ULONG64 address;
		USHORT* item;
		ULONG32 count;
	};

	struct import_function_info
	{
		std::string name;
		ULONG64* address;
	};

	struct import_info
	{
		std::string module_name;
		std::vector< import_function_info > function_datas;
	};

	using vec_sections = std::vector< IMAGE_SECTION_HEADER >;
	using vec_relocs = std::vector< reloc_info >;
	using vec_imports = std::vector< import_info >;

	PIMAGE_NT_HEADERS64 get_nt_headers( void* image_base );
	vec_relocs get_relocs( void* image_base );
	vec_imports get_imports( void* image_base );
};
