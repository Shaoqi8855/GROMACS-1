MACRO(gmx_c_flags)

include(CheckCCompilerFlag)

if( CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX )
  CHECK_C_COMPILER_FLAG( "-O3" XFLAGS_O3)
  IF (XFLAGS_O3)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
  ENDIF(XFLAGS_O3)

  CHECK_C_COMPILER_FLAG( "-Wall -Wno-unused" XFLAGS_WARN)
  IF (XFLAGS_WARN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-unused")
  ENDIF(XFLAGS_WARN)
  CHECK_C_COMPILER_FLAG( "-std=gnu99" XFLAGS_GNU99)
  IF (XFLAGS_GNU99)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
  ENDIF(XFLAGS_GNU99)

  CHECK_C_COMPILER_FLAG( "-march=native" XFLAGS_MARCH)
  IF (XFLAGS_MARCH)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
  ENDIF(XFLAGS_MARCH)
endif( CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX )

MARK_AS_ADVANCED(XFLAGS_O3 XFLAGS_WARN XFLAGS_GNU99 XFLAGS_MARCH)

ENDMACRO(gmx_c_flags)
