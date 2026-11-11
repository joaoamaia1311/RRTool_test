#pragma once

#if defined( _KERNEL_MODE ) || defined( _NTDDK_ ) || defined( _WDMDDK_ )
#include <ntdef.h>
#define CHANNEL_KERNEL 1
#else
#include <cstdint>
#endif

namespace channel
{
#if defined( CHANNEL_KERNEL )
	using u8 = UINT8;
	using u32 = UINT32;
	using u64 = UINT64;
	using i32 = INT32;
#else
	using u8 = std::uint8_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using i32 = std::int32_t;
#endif

	inline constexpr u64 handshake_magic = 0x544F4C534C4F54ULL;
	inline constexpr u64 block_magic = 0x4B434E4C43484ELL;
	inline constexpr u64 session_magic = 0x5353504F4C53ULL;
	inline constexpr u32 protocol_version = 1;
	inline constexpr u32 slot_count = 8;
	inline constexpr u32 payload_capacity = 0xFE0;

	enum class slot_state : u32
	{
		idle = 0,
		queued = 1,
		processing = 2,
		done = 3,
	};

	enum class command : u32
	{
		none = 0,
		ping = 1,
		read = 2,
		write = 3,
		get_dirbase = 4,
		get_image_base = 5,
		eac_find = 6,
		eac_get_base = 7,
		eac_scan = 8,
		sandbox_enable = 9,
		eac_disable = 10,
		sandbox_pending = 11,
		eac_dequeue = 12,
		sandbox_inject = 13,
	};

	struct request_slot
	{
		volatile u32 state;
		u32 command_xor;
		u64 address;
		u64 size;
		u64 pid;
		u64 buffer;
		u64 result;
		i32 status;
		u32 _pad;
	};

	struct shared_block
	{
		u64 magic;
		u64 version;
		u64 session_key;
		volatile u64 heartbeat;
		volatile u32 write_index;
		u32 _pad0;
		request_slot slots[ slot_count ];
		u8 payload[ payload_capacity ];
	};

	struct map_handshake
	{
		u64 magic;
		u32 client_pid;
		u32 bootstrap_pid;
		u64 shared_user_va;
		u64 shared_kernel_va;
		u64 session_key;
		u64 driver_image_base;
	};

	struct session_file
	{
		u64 magic;
		u32 owner_pid;
		u32 reserved;
		u64 shared_user_va;
		u64 session_key;
		u64 driver_image_base;
	};

	inline u32 encode_command( command cmd, u64 session_key )
	{
		return static_cast< u32 >( cmd ) ^
			static_cast< u32 >( session_key ) ^
			static_cast< u32 >( session_key >> 32 );
	}

	inline command decode_command( u32 encoded, u64 session_key )
	{
		return static_cast< command >(
			encoded ^
			static_cast< u32 >( session_key ) ^
			static_cast< u32 >( session_key >> 32 ) );
	}
}
