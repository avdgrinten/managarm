#pragma once

#include <string>
#include <string_view>

std::string toLower(std::string_view s);

bool hasManagarmRootAttribute(const std::string &device);

std::string findDevNode(const std::string &device);
