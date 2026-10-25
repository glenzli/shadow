#pragma once

#include "../edit_history.hpp"
#include "../edit_history_snapshot.hpp"

using DocumentUndo = SessionEditHistory<EditHistorySnapshot>::Archive;
using DocumentHistoryEntry = SessionEditHistory<EditHistorySnapshot>::Entry;
