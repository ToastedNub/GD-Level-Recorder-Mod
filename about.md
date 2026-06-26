# Level Recorder
 - A native rendering hook to pipe raw OpenGL frames directly to FFmpeg for lossless recordings!

## Features
 - Hooks directly into FMOD and CCDirector for perfect sync.
 - Completely lossless capture using raw pipe streams.
 - Records Full Sessions, not single runs (files can get big though).

## Requirements
 - You need to Download FFMPEG and put ffmpeg.exe it in the Mod Folder.
 - Download: https://www.gyan.dev/ffmpeg/builds/
 - Scroll down in that page to "Release Builds", download "ffmpeg-release-essentials.7z".
 - Go into the zip it downloads, and into the "bin" folder
 - Drag or Copy "ffmpeg.exe" into the mod folder, you can see the directory to your mod folder by clicking Start on the mods settings page before adding ffmpeg to it.

## CRF INFO
 - Since this uses FFMPEG, You can choose your quality level with CRF.
 - 0 is mathematically lossless, but will not be viewable after created, can still be uploaded and edited though.
 - 1-12 is VISUALLS lossless, but will have a much bigger file size.
 - 13-24 is recommended, it is not lossless, but is close enough to be considered it (sometimes).
 - Anything over 24 will lose quality rapidly (max value is 63 (for meme compression, etc)).

 - Lower CRF Value = More Quality, and Bigger FIle Size.

## How It Works
 - Go into a Level.
 - Pause, click the Big Red Settings Icon.
 - Set your FPS, and your CRF values, and your Recording Path.
 - START and STOP are top stop and start the recording.

 - The recording will be the resolution of your Game Window.
 - It is highly recommended to not go over 60fps for recordings, as it can lag a lot.
 - If you want a lower CRF Value (0-12), use another mod to slow down the game, this will help to prevent lag, lowering the Target FPS wont do much, unless you are on a slower system.

## Known Issues
 - When you Start a Recording, let it go for about 5 seconds, just to make sure ffmpeg fully starts, and doesnt crash, this can crash the game if you dont wait.
 - Sometimes the First Recording will have Delayed Audio, make a 5-10 second recording just to get ffmpeg loaded, then the next ones will work fine.
 - When you Stop Recording, wait a few seconds before you open the pause menu to do this, just to make sure ffmpeg loads all the frames it needs, into the video file.

# Extras
 - If you need extra tools, for editing the output video in any way, i have made tons of public apps you can use, all open source, and free, and made to be very easy to use, and set up.
 - You can find those apps here: https://github.com/ToastedNub/ScriptMenu/releases/tag/ScriptMenu.