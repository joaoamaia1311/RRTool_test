#pragma once
#include <framework/portable_executable/portable_executable.hxx>
#include <windows.h>

namespace mapper
{
	enum class allocation_mode
	{
		allocate_pool,
		allocate_independent_pages,
	};

	typedef bool ( *map_callback )(
		ULONG64* param_one,
		ULONG64* param_two,
		ULONG64 allocation_pointer,
		ULONG64 allocation_size );

	void relocate_image_by_delta( portable_executable::vec_relocs relocs, const ULONG64 delta );
	bool fix_security_cookie( void* local_image, ULONG64 kernel_image_base );
	bool resolve_imports( portable_executable::vec_imports imports );
	ULONG64 map_driver(
		BYTE* data,
		ULONG64 param1,
		ULONG64 param2,
		bool free_memory,
		bool destroy_header,
		allocation_mode mode,
		bool pass_allocation_address_as_first_param,
		map_callback callback,
		NTSTATUS* exit_code );
};
