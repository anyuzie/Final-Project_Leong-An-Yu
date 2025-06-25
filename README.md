# Final Project
Completed by 

    Name: Leong An Yu
    Matriculation Number: 12446374

## Project Description
In order to escape, the player needs to gather all collectibles. If the player runs into the Zombie or the Ghost, the player will no longer be able to move.

## Project Features
1. Features included in client
    * Allows players to control the movement of their character
    * Change the lighting set up 
    * Monitor their game progress
2. Features included in server
    * Background music
    * Collision Detection & Activation of sound effects upon collision
    * Encoding of FFMPEG
    * Sending of multiple data streams via UDP

## Set up
things to do when copying template:
1. in `template\build_MSVC.cmd`
    * replace `template` with the name of your folder in `..\template\build\%CMAKE_BUILD_TYPE%\game.exe`
2. in `game.cpp`
    * replace `template` with the name of your folder in `m_ffmpegWriter = std::make_unique<FFmpegWriter>(extent.width, extent.height, "C:/Users/aanny/source/repos/fork/template/videos/recording.h264");`

## Overview of Repository
    | - escape

        | - game.cpp
        | - assets
        | - videos

    | - receiver.cpp

    | - SDL3.dll

    | - stb_image_write.h

to convert h264 to mp4:

`ffmpeg -i recording.h264 -c:v copy output.mp4`

to start receiver.cpp:

`g++ receiver.cpp -o receiver.exe ^
  -IC:/ffmpeg/include -IC:/SDL3/x86_64-w64-mingw32/include ^
  -LC:/ffmpeg/lib -LC:/SDL3/x86_64-w64-mingw32/lib ^
  -lavcodec -lavutil -lswscale -lSDL3 -lws2_32`
