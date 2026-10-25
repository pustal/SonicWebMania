# ReportFootprint.cmake
#
# Script mode helper (cmake -P) run after packaging. Prints how much space the
# app needs on the TV, so the numbers in README.md stay honest.
#
# Variables: STAGE (staged IPK directory), IPK (built package path).

file(GLOB_RECURSE staged_files LIST_DIRECTORIES false "${STAGE}/*")
set(installed_bytes 0)
foreach(path IN LISTS staged_files)
    file(SIZE "${path}" size)
    math(EXPR installed_bytes "${installed_bytes} + ${size}")
endforeach()

set(ipk_bytes 0)
if(EXISTS "${IPK}")
    file(SIZE "${IPK}" ipk_bytes)
endif()

math(EXPR installed_kib "${installed_bytes} / 1024")
math(EXPR ipk_kib "${ipk_bytes} / 1024")

message(STATUS "IPK download size:     ${ipk_kib} KiB")
message(STATUS "Installed app size:    ${installed_kib} KiB")
message(STATUS "Plus at runtime:       your Data.rsdk, saves and settings (well under 1 MiB;")
message(STATUS "                       time-attack replays add a little), and 32 MiB kept free during uploads.")
