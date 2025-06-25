rem git clone -b nexgen.develop https://github.com/hlavacs/ViennaVulkanEngine.git
rem cd into VVE and build it with MSVC
rem then cd int the game directory 

set CMAKE_BUILD_TYPE=Debug
cmake -S . -Bbuild -A x64 -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%
cmake --build build --config %CMAKE_BUILD_TYPE%

rem run game from VVE directory!
cd ..\ViennaVulkanEngine
..\escape\build\%CMAKE_BUILD_TYPE%\game.exe
