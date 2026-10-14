---@meta
-- LuaLS classes shared by every zephlet type: the frontend's own objects and
-- the messages of the shared zephlet.proto. Copied next to the generated
-- per-type files after each build.

---@class zephlets.EventMeta
---@field source table   the instance that emitted the event
---@field name string    its name
---@field seq integer    per-instance event number, counting dropped events
---@field dropped integer events of this instance lost since the last one delivered
---                        to this consumer

---@class zephlets.Stats
---@field emitted integer       events the instance emitted
---@field delivered integer     events handed to its handlers
---@field staged_drop integer   dropped: every staging slot taken
---@field fifo_drop integer     dropped: Lua-heap FIFO full, or out of memory
---@field doorbell_fail integer doorbells the zbus message pool refused
---@field handler_err integer   handler calls that raised
---@field await_drop integer    dropped: an awaiting task's queue full, or out of memory

---@class pb.Empty

---@class pb.Lifecycle.Status
---@field is_running boolean
---@field is_ready boolean
