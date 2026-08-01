# Embed a text file as a byte array in a generated C header, so the runtime
# shader compiler (D3DCompile + a custom ID3DInclude) can serve the FidelityFX
# headers from inside the DLL instead of needing files on disk next to the game.
function(embed_text_file input output symbol)
    file(READ "${input}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${output}"
        "// Auto-generated from ${input} -- do not edit.\n"
        "static const unsigned char ${symbol}[] = {${bytes}0x00};\n"
        "static const unsigned int ${symbol}_len = sizeof(${symbol}) - 1;\n")
endfunction()
