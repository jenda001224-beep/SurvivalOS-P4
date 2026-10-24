# eSpeak NG (GPL-3.0-or-later, https://github.com/espeak-ng/espeak-ng), the
# library part only, for the Translator app's speech. Commit in UPSTREAM_COMMIT.
set(srcs
    src/common.c
    src/mnemonics.c
    src/error.c
    src/ieee80.c
    src/compiledata.c
    src/compiledict.c
    src/dictionary.c
    src/encoding.c
    src/intonation.c
    src/langopts.c
    src/numbers.c
    src/phoneme.c
    src/phonemelist.c
    src/readclause.c
    src/setlengths.c
    src/soundicon.c
    src/spect.c
    src/ssml.c
    src/synthdata.c
    src/synthesize.c
    src/tr_languages.c
    src/translate.c
    src/translateword.c
    src/voices.c
    src/wavegen.c
    src/speech.c
    src/espeak_api.c
    src/klatt.c
    ucd/case.c
    ucd/categories.c
    ucd/ctype.c
    ucd/proplist.c
    ucd/scripts.c
    ucd/tostring.c
)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       LDFRAGMENTS "linker.lf"
                       PRIV_INCLUDE_DIRS "include/espeak-ng" "config" "ucd" "src")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "LIBESPEAK_NG_EXPORT=1"
    "PATH_ESPEAK_DATA=\"/sdcard/translator/espeak-ng-data\"")
target_compile_definitions(${COMPONENT_LIB} INTERFACE "LIBESPEAK_NG_EXPORT=1")
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fwrapv
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-missing-prototypes
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-format-truncation
    -Wno-format
    -Wno-char-subscripts
    -Wno-maybe-uninitialized
    -Wno-pointer-sign
    -Wno-missing-field-initializers
    -Wno-old-style-declaration
    -Wno-stringop-truncation
    -Wno-misleading-indentation
    -Wno-dangling-pointer
    -Wno-error)
