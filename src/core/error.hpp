#pragma once

#include <stdexcept>
#include <string>

namespace skeleton_repair {

class InvalidData : public std::runtime_error {
public:
    explicit InvalidData(const std::string& message) : std::runtime_error(message) {}
};

class IoError : public std::runtime_error {
public:
    explicit IoError(const std::string& message) : std::runtime_error(message) {}
};

class InvalidOperation : public std::runtime_error {
public:
    explicit InvalidOperation(const std::string& message) : std::runtime_error(message) {}
};

class OperationCancelled : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Cancelled.") {}
};

}  // namespace skeleton_repair
