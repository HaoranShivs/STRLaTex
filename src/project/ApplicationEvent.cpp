#include "project/ApplicationEvent.h"

#include <type_traits>

namespace pf {

const char* ToString(const ApplicationEvent& event) {
    return std::visit(
        [](const auto& e) -> const char* {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, BuildPhaseChangedEvent>) {
                return "BuildPhaseChanged";
            } else if constexpr (std::is_same_v<T, BuildResultReadyEvent>) {
                return "BuildResultReady";
            } else if constexpr (std::is_same_v<T, BuildEventReadyEvent>) {
                return "BuildEventReady";
            } else if constexpr (std::is_same_v<T, SaveCompletedEvent>) {
                return "SaveCompleted";
            } else {
                return "AutosaveTick";
            }
        },
        event);
}

}  // namespace pf
