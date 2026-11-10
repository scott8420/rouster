#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Compare (s017b): what the side-by-side versions viewer needs, GTK-free and
// READ-ONLY (it opens files to read them, nothing else).
//
//   kind_of()   -- how a save is shown: a picture, a text, one of our apps'
//                  files, or just its date and size.
//   read_text() -- the words of a text: plain text as it is (bad UTF-8
//                  replaced, never trusted), an ODT / OTT / DOCX's paragraphs
//                  out of its zip (content.xml / word/document.xml, inflated
//                  with zlib), a flat FODT's paragraphs. One paragraph = one line.
//   rows()      -- the two texts aligned for the viewer: unchanged lines side by
//                  side, a changed run paired line by line, an added or removed
//                  line against a blank. A built-in line diff (Myers), not Plunk.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::compare {

enum class Kind { Image, Text, App, Other };

// By the lower-case extension ("odt", "folio.gz", ...) and whether it's a
// package; an unknown extension on a small file is sniffed (no NUL, mostly
// UTF-8 -> Text). `path` is only read for the sniff.
Kind kind_of(const std::string& path, const std::string& ext, bool package);
// "Curvz" for .curvz, "Folio" for .folio, "jot" for .jots, "Optik" for .optik;
// "" for anything else.
std::string app_of(const std::string& ext);

inline constexpr std::uint64_t kTextCap  = 8ull << 20;    // bytes read from a plain text / inflated from a zip member
inline constexpr std::size_t   kLineCap  = 20000;         // lines kept

struct Text {
    bool                     ok = false;
    std::string              why;     // when !ok: "not a text", "too big", "the zip has no content.xml", ...
    std::vector<std::string> lines;   // valid UTF-8, no '\n', no '\r'
    bool                     cut = false;   // over kLineCap / kTextCap -- the rest not shown
};
Text read_text(const std::string& path, const std::string& ext);

// One member of a zip (stored or deflated; no zip64), at most `cap` bytes.
// Empty + `why` if it can't be had.
std::string zip_member(const std::string& zip_path, const std::string& name, std::uint64_t cap, std::string* why);
// The paragraphs of an ODF body (content.xml, or a whole .fodt) / a DOCX body.
std::vector<std::string> odf_lines(const std::string& xml);
std::vector<std::string> docx_lines(const std::string& xml);
// Bad UTF-8 bytes -> U+FFFD; NUL -> U+FFFD.
std::string clean_utf8(const std::string& s);

// ── the diff ─────────────────────────────────────────────────────────────────
enum class Mark { Same, Changed, Removed, Added, Gap };
struct Row {
    Mark        mark = Mark::Same;
    std::string left, right;     // Removed: right empty; Added: left empty
    std::size_t lno = 0, rno = 0; // 1-based line numbers (0 = none on that side)
    std::size_t skipped = 0;      // Gap: how many same lines are folded here
};
struct Rows {
    std::vector<Row> rows;
    std::size_t      same = 0, changed = 0, removed = 0, added = 0;
    bool             too_different = false;   // past the edit budget: shown side by side, not marked
    bool             identical() const { return !too_different && !changed && !removed && !added; }
};
inline constexpr std::size_t kEditBudget = 2000;   // Myers' D -- beyond it, too different to mark (bounds memory ~D^2)
// `left` = the keeper, `right` = the version. With `context` > 0, a run of
// same lines longer than 2*context+1 is folded to its first and last `context`
// lines around one Gap row; 0 = every line.
Rows rows(const std::vector<std::string>& left, const std::vector<std::string>& right, std::size_t context);

}  // namespace rouster::core::compare
