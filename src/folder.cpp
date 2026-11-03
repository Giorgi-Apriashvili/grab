#include "folder.hpp"

namespace grab::folder {

std::vector<std::size_t> visible(const std::vector<State>& states, std::size_t queued_limit) {
    std::vector<std::size_t> out;
    std::size_t queued = 0;
    for (std::size_t i = 0; i < states.size(); ++i) {
        switch (states[i]) {
        case State::running:
        case State::paused:
        case State::failed:
            out.push_back(i);
            break;
        case State::queued:
            if (queued++ < queued_limit) out.push_back(i);
            break;
        case State::done:
        case State::skipped:
            break;
        }
    }
    return out;
}

} // namespace grab::folder
