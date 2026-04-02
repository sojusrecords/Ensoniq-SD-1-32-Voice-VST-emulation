/* =========================================================================================

   This is an auto-generated file: Any edits you make may be overwritten!

*/

#pragma once

namespace BinaryData
{
    extern const char*   q_svg;
    const int            q_svgSize = 682;

    extern const char*   cart_svg;
    const int            cart_svgSize = 1153;

    extern const char*   floppy_svg;
    const int            floppy_svgSize = 1457;

    extern const char*   labels_tablet_png;
    const int            labels_tablet_pngSize = 35964;

    extern const char*   labels_rack_png;
    const int            labels_rack_pngSize = 18137;

    extern const char*   labels_compact_png;
    const int            labels_compact_pngSize = 20672;

    extern const char*   labels_full_png;
    const int            labels_full_pngSize = 18978;

    // Number of elements in the namedResourceList and originalFileNames arrays.
    const int namedResourceListSize = 7;

    // Points to the start of a list of resource names.
    extern const char* namedResourceList[];

    // Points to the start of a list of resource filenames.
    extern const char* originalFilenames[];

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding data and its size (or a null pointer if the name isn't found).
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding original, non-mangled filename (or a null pointer if the name isn't found).
    const char* getNamedResourceOriginalFilename (const char* resourceNameUTF8);
}
