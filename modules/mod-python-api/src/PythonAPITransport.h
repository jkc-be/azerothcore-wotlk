#ifndef MOD_PYTHON_API_TRANSPORT_H
#define MOD_PYTHON_API_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PythonAPI
{
struct Command
{
    uint64_t client = 0;
    uint64_t id = 0;
    uint64_t bot = 0;
    uint64_t target = 0;
    uint32_t spell = 0;
    uint32_t waitTicks = 1;
    float x = 0;
    float y = 0;
    float z = 0;
    std::string operation;
};

// Network threads exchange value objects only. No gameplay objects cross this boundary.
class Transport
{
public:
    Transport();
    ~Transport();
    bool Start(uint16_t port);
    void Stop();
    uint64_t Client() const;
    std::vector<Command> Drain();
    void Reply(uint64_t client, std::string message);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}

#endif
