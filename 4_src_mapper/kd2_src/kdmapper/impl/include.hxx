#pragma once
#include <iostream>
#include <windows.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

namespace mapper
{
	inline bool logs_enabled = true;
}

#define MAPPER_LOG( expr ) \
	do { \
		if ( mapper::logs_enabled ) \
			std::cout expr; \
	} while ( 0 )

#include <protection/sk_crypt.hxx>

#include <nt/nt.hxx>
#include <resource/resource.hxx>

#include <framework/util/util.hxx>
#include <framework/portable_executable/portable_executable.hxx>
#include <intel/service/service.hxx>
#include <intel/driver/driver.hxx>
#include <framework/mapper/mapper.hxx>
