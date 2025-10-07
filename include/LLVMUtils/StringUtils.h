#ifndef UTILS_STRINGUTILS_H
#define UTILS_STRINGUTILS_H

#include <cstdio>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>

using namespace llvm;

// An immediate-to-use string formatting function
template <typename... Args>
std::string format_str(const char *format, Args... args) {
  // First we try it with a small stack buffer
  const int BUF_SIZE = 256;
  char stack_buf[BUF_SIZE];
  size_t size = snprintf(stack_buf, BUF_SIZE, format, args...) + 1;
  if (size <= BUF_SIZE)
    return std::string(stack_buf);

  // Second we try it with a dynamic memory
  std::unique_ptr<char[]> buf(new char[size]);
  snprintf(buf.get(), size, format, args...);
  return std::string(buf.get());
}

// Store the formated string into a string
template <typename... Args>
void format_str(std::string &res_str, const char *format, Args... args) {
  // First we try it with a small stack buffer
  const int BUF_SIZE = 256;
  char stack_buf[BUF_SIZE];
  size_t size = snprintf(stack_buf, BUF_SIZE, format, args...) + 1;

  if (size <= BUF_SIZE) {
    res_str.assign(stack_buf);
  } else {
    // Scale the string to enough memory size and try again.
    res_str.resize(size);
    // Directly writing to string internal buffer is not a good practice, but
    // very efficient
    snprintf(const_cast<char *>(res_str.data()), size, format, args...);
  }
}

// The string is allocated with customized allocator
// Please include Allocator.h to use this function, otherwise it gets compile
// error
template <class Allocator, typename... Args>
StringRef hooked_format_str(Allocator &A, const char *fmt, Args... args) {
  // First we try it with a small stack buffer
  const int BUF_SIZE = 256;
  char stack_buf[BUF_SIZE];
  size_t size = snprintf(stack_buf, BUF_SIZE, fmt, args...) + 1;
  char *buffer = A.template Allocate<char>(size);

  if (size <= BUF_SIZE) {
    // String copy is faster than calling snprintf again
    strcpy(buffer, stack_buf);
  } else {
    snprintf(buffer, size, fmt, args...);
  }

  // Don't count the trailing zero, though the trailing zero is already there
  // This is because printing the StringRef with trailing zero may have a
  // strange character
  return StringRef(buffer, size - 1);
}

// Copy the input string into a char array
// Please include Allocator.h to use this function, otherwise it gets compile
// error
template <class Allocator>
StringRef copy_string(const std::string &str, Allocator &A) {
  int size = str.size();
  char *new_str = A.template Allocate<char>(size + 1);
  strncpy(new_str, str.data(), size);
  new_str[size] = 0;
  return StringRef(new_str, size);
}

// Escape the meta-characters in the string
std::string html_escape_string(const std::string &);

// Construct a markdown style emphasis of given string
inline std::string emph_str(const std::string &str) {
  return "<b>" + str + "</b>";
}

// A markdown style italic
inline std::string italic_str(const std::string &str) {
  return "<i>" + str + "</i>";
}

inline std::string color_str(const std::string &str, const std::string color) {
  return "<font color=" + color + ">" + str + "</font>";
}

// Remove the string \p prefix from the prefix of string \p str
// Return true if something is removed
bool remove_prefix(StringRef &str, StringRef prefix);

// Draw a separate line with character \p sep at length \p l to the stream O
// If \p end_line is true, a \n will be placed at the end
void draw_separate_line(raw_ostream &O, int l, char sep = '-',
                        bool end_line = true);

/*
 * Following three functions output a middle, left, and right aligned string \p
 * text. The string is padded to length \p l with char \p padc. End the line if
 * \p end_line is true
 */
void output_padded_text(raw_ostream &O, StringRef text, int l, char padc = ' ',
                        bool end_line = true);
void output_left_aligned_text(raw_ostream &O, StringRef text, int l,
                              char padc = ' ', bool end_line = true);
void output_right_aligned_text(raw_ostream &O, StringRef text, int l,
                               char padc = ' ', bool end_line = true);

// Returns either "st", "nd", "rd" or "th" depending on the number.
// e.g. int t = 21; printf("%d%s", t, ordinalSuffix(t));
// will print: "21st"
const char *ordinal_suffix(int n);

// a lazyman helper function.
std::string ordinal_string(int n);

// return the binary string of the parameter
std::string to_binary_string(int n);

// Bool to string
inline const char *bool_to_string(bool b) { return b ? "true" : "false"; }

#endif /* UTILS_STRINGUTILS_H */