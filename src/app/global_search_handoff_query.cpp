#include "global_search_handoff.h"
#include "search_query.h"
#include "places.h"

namespace pulse {
std::wstring GlobalSearchHandoffPath(const GlobalSearchHandoff& request) {
    if (request.query.empty()) return {};
    app::AdvancedSearchSpec spec;
    if (request.content) spec.content = request.query;
    spec.current_folder = request.folder;
    spec.location = request.folder.empty() ? app::LocationScope::Indexed : app::LocationScope::CurrentFolder;
    auto query = app::CompileSearchQuery(spec);
    if (!request.content) {
        const auto raw = !request.folder.empty() && !index::ParseQuery(request.query).path_prefix.empty()
            ? index::QueryWithoutPathPrefix(request.query) : request.query;
        query = raw + (query.empty() ? L"" : L" " + query);
    }
    return app::MakeSearchPath(query);
}

} // namespace pulse
