#include "OpdsFilename.h"

#include "StringUtils.h"

std::string opdsBookFilename(const std::string& author, const std::string& title, const OpdsFilenameFormat format) {
  std::string base;
  switch (format) {
    case OpdsFilenameFormat::TitleAuthor:
      base = author.empty() ? title : title + " - " + author;
      break;
    case OpdsFilenameFormat::TitleOnly:
      base = title;
      break;
    case OpdsFilenameFormat::AuthorTitle:
    default:
      base = author.empty() ? title : author + " - " + title;
      break;
  }
  return StringUtils::sanitizeFilename(base) + ".epub";
}
