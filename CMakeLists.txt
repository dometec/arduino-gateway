project(arduinogateway)

add_executable(arduinogateway main.c)

install(TARGETS arduinogateway RUNTIME DESTINATION bin)
