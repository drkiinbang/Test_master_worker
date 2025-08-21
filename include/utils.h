#pragma once
#include <string>
#include <vector>

std::string ltrim(const std::string& s);
std::string rtrim(const std::string& s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string escapeDQ(const std::string& s);