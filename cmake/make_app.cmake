# Assembles Shogo.app from the built pieces (run by the shogo-app target):
#   cmake -DAPP=... -DEXE=... -DOBJECT=... -DCSHELL=... -DSDL=... -DVERSION=... [-DMINOS=...] -P make_app.cmake
# MINOS: the macOS the build targets (CMAKE_OSX_DEPLOYMENT_TARGET), so an older
# one says the app needs a newer macOS instead of failing to load it.
if(NOT MINOS)
    set(MINOS 13.0)
endif()
# The app holds the engine and our compiled build of Monolith's game code
# (a "variation" under the source licence); never the game's data, which the
# launcher imports from the player's own GOG installer.
file(REMOVE_RECURSE "${APP}")
file(MAKE_DIRECTORY "${APP}/Contents/MacOS" "${APP}/Contents/Frameworks")
file(COPY "${EXE}" "${OBJECT}" "${CSHELL}" DESTINATION "${APP}/Contents/MacOS")

# SDL3 goes inside the app, so it runs without Homebrew.
file(REAL_PATH "${SDL}" sdl_real)
file(COPY_FILE "${sdl_real}" "${APP}/Contents/Frameworks/libSDL3.0.dylib")
execute_process(COMMAND otool -L "${APP}/Contents/MacOS/shogo" OUTPUT_VARIABLE deps)
string(REGEX MATCH "[^\t\n ]*libSDL3[^\t\n ]*\\.dylib" sdl_ref "${deps}")
if(NOT sdl_ref)
    message(FATAL_ERROR "shogo doesn't link SDL3?")
endif()
execute_process(COMMAND install_name_tool -id "@rpath/libSDL3.0.dylib" "${APP}/Contents/Frameworks/libSDL3.0.dylib" ERROR_QUIET)
execute_process(COMMAND install_name_tool -change "${sdl_ref}" "@executable_path/../Frameworks/libSDL3.0.dylib"
                        "${APP}/Contents/MacOS/shogo")

file(WRITE "${APP}/Contents/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleExecutable</key><string>shogo</string>
<key>CFBundleIdentifier</key><string>com.eriken.shogo-recomp</string>
<key>CFBundleName</key><string>Shogo</string>
<key>CFBundleDisplayName</key><string>Shogo: Mobile Armor Division</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>${VERSION}</string>
<key>CFBundleVersion</key><string>${VERSION}</string>
<key>LSMinimumSystemVersion</key><string>${MINOS}</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
</dict></plist>
")
# Ad-hoc signature (Apple silicon needs one; install_name_tool broke the old).
execute_process(COMMAND codesign --force --deep -s - "${APP}" RESULT_VARIABLE sign_result OUTPUT_QUIET ERROR_QUIET)
if(NOT sign_result EQUAL 0)
    message(WARNING "codesign failed: the app may not start")
endif()
