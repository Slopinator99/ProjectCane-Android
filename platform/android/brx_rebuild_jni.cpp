// JNI entry for the on-device importer: runs upstream's BRX rebuilder (tools/BrxRebuilder) on a decompressed level so the
// game gets the PCBRX v17 file it requires (the desktop extractor spawns Sly1BrxRebuilder for the same job).
#include <jni.h>

#include <exception>
#include <filesystem>
#include <string>

#include "BrxWalker.h"

// Returns null on success (output is "<stem>_Rebuilt.brx" beside the input) or an error message.
extern "C" JNIEXPORT jstring JNICALL
Java_org_projectcane_cane_extract_BrxRebuild_nativeRebuild(JNIEnv* env, jclass, jstring jpath)
{
    const char* utf = env->GetStringUTFChars(jpath, nullptr);
    const std::filesystem::path input(utf);
    env->ReleaseStringUTFChars(jpath, utf);

    try {
        if (!std::filesystem::is_regular_file(input) || input.extension() != ".brx")
            throw std::runtime_error("input must be an existing .brx file");
        BrxWalker walker(StringEncoding::Narrow);
        walker.WalkFile(input);
        return nullptr;
    }
    catch (const std::exception& e) {
        return env->NewStringUTF(e.what());
    }
    catch (...) {
        return env->NewStringUTF("unknown error while rebuilding the level");
    }
}
