#include "LLVMUtils/StringUtils.h"

using namespace std;

// Removes the given prefix from the string if present. Returns true if removed.
bool remove_prefix(StringRef &str, StringRef prefix) {
  if (str.startswith(prefix)) {
    str = str.drop_front(prefix.size());
    return true;
  }

  return false;
}

// Draws a line of the given length using the specified character.
static void draw_line_with(raw_ostream &O, int length, char sym) {
  const int BUF_SIZE = 64;
  char buf[BUF_SIZE];

  int times = length / (BUF_SIZE - 1);
  int remainder = length - (BUF_SIZE - 1) * times;

  // Fill the buffer quickly
  memset(buf, sym, BUF_SIZE - 1);
  buf[BUF_SIZE - 1] = 0;

  // Batch output to save time for generating long separate line
  for (; times > 0; --times)
    O << buf;

  // Output the remaining section
  buf[remainder] = 0;
  O << buf;
}

// Draws a separator line with optional newline.
void draw_separate_line(raw_ostream &O, int length, char sep, bool end_line) {
  draw_line_with(O, length, sep);
  if (end_line)
    O << "\n";
}

// Outputs text centered with padding characters.
void output_padded_text(raw_ostream &O, StringRef text, int l, char padc,
                        bool end_line) {
  int padding_size = l - text.size();
  if (padding_size <= 0) {
    O << text << "\n";
    return;
  }

  draw_line_with(O, padding_size / 2, padc);
  O << text;
  draw_line_with(O, (padding_size + 1) / 2, padc);
  if (end_line)
    O << "\n";
}

// Outputs text left-aligned with padding.
void output_left_aligned_text(raw_ostream &O, StringRef text, int l, char padc,
                              bool end_line) {
  O << text;
  int padding_size = l - text.size();
  if (padding_size > 0)
    draw_line_with(O, padding_size, padc);
  if (end_line)
    O << "\n";
}

// Outputs text right-aligned with padding.
void output_right_aligned_text(raw_ostream &O, StringRef text, int l, char padc,
                               bool end_line) {
  int padding_size = l - text.size();
  if (padding_size > 0)
    draw_line_with(O, padding_size, padc);
  O << text;
  if (end_line)
    O << "\n";
}

// Returns the ordinal suffix (st, nd, rd, th) for a number.
const char *ordinal_suffix(int n) {
  // make n always positive. This is faster than having to check for
  // negative cases.
  n = n < 0 ? -n : n; // same as n = abs(n); but faster, so there.

  // Numbers from 11 to 13 don't have st, nd, rd
  if (10 < n && n < 14)
    return "th";

  switch (n % 10) {
  case 1:
    return "st";

  case 2:
    return "nd";

  case 3:
    return "rd";

  default:
    return "th";
  }
}

// Returns a number with its ordinal suffix as a string.
std::string ordinal_string(int n) {
  return format_str("%d%s", n, ordinal_suffix(n));
}

// Converts an integer to its binary string representation.
std::string to_binary_string(int n) {
  std::string r;
  while (n != 0) {
    r.insert(0, (n % 2 == 0 ? "0" : "1"));
    n /= 2;
  }
  return r;
}

// Escapes special characters in a string for HTML output.
string html_escape_string(const string &data) {
  std::string buffer;
  buffer.reserve(data.size() * 1.5);

  for (size_t pos = 0; pos != data.size(); ++pos) {
    switch (data[pos]) {
    case ' ':
      buffer.append("&nbsp;");
      break;
    case '&':
      buffer.append("&amp;");
      break;
    case '\"':
      buffer.append("&quot;");
      break;
    case '\'':
      buffer.append("&#39;");
      break;
    case '<':
      buffer.append("&lt;");
      break;
    case '>':
      buffer.append("&gt;");
      break;
    default:
      buffer.append(&data[pos], 1);
      break;
    }
  }

  return buffer;
}