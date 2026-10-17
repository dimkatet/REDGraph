-- REDGraph - the render host's CET window (src/ui.cpp). Its settings are REDGraph.ini's, named "Section.Key",
-- read with Game.RG_Get(name) and written with Game.RG_Set(name, value). The render graph debug window is here.
-- No text below may contain a percent sign: CET's ImGui text calls treat their string as a format.

local S = {}            -- the host's values, by name
local plugin = nil      -- the RG_Ping string, nil = REDGraph is not there
local overlay = false
local status = ""
local G                 -- the render graph debug window's state (below)

local kNames = { "Diagnostics.VerboseLog", "Diagnostics.RenderGraphDebug", "Diagnostics.RenderGraphDump", "Diagnostics.Dred",
                 "Diagnostics.GpuTiming", "Diagnostics.BlasCensus", "Diagnostics.FrameTimer" }

local function log(text) print("[REDGraph] " .. text) end

local function refresh()
    local ok, ping = pcall(function() return Game.RG_Ping() end)
    plugin = ok and ping or nil
    if not plugin then
        log("REDGraph is not loaded: " .. tostring(ping))
        return
    end
    for _, n in ipairs(kNames) do S[n] = Game.RG_Get(n) end
end

local function set(name, value)
    if not plugin then return end
    local ok, res = pcall(function() return Game.RG_Set(name, value) end)
    if not ok or not res then log("RG_Set(" .. name .. ", " .. tostring(value) .. ") failed: " .. tostring(res)) end
    S[name] = Game.RG_Get(name)
end

local function on(name) return (S[name] or 0) > 0.5 end

local function tip(text)
    if ImGui.IsItemHovered() then ImGui.SetTooltip(text) end
end

local function check(label, name, text)
    local v, pressed = ImGui.Checkbox(label, on(name))
    if pressed then set(name, v and 1 or 0) end
    tip(text)
    return on(name)
end

local function radio(name, labels, text)
    for i, label in ipairs(labels) do
        if i > 1 then ImGui.SameLine() end
        if ImGui.RadioButton(label, math.floor((S[name] or 0) + 0.5) == i - 1) then set(name, i - 1) end
        tip(text)
    end
end

local function slider(label, name, lo, hi, fmt, text)
    local v, used = ImGui.SliderFloat(label, S[name] or lo, lo, hi, fmt)
    if used and v ~= S[name] then set(name, v) end
    if text then tip(text) end
end

local function heading(text)
    ImGui.Spacing()
    ImGui.Separator()
    ImGui.Text(text)
end

local function note(text) ImGui.TextDisabled("  " .. text) end

-- ---- the host's own CPU cost, on a timer (core/prof.hpp) ----

local P = { delay = 10, seconds = 30, msg = "", state = "" }

local function draw_profile()
    heading("Host CPU profile")
    ImGui.SetNextItemWidth(110)
    P.delay = ImGui.InputInt("delay, s", P.delay)
    tip("Time to close this overlay and leave the game untouched before the measurement starts.")
    ImGui.SameLine()
    ImGui.SetNextItemWidth(110)
    P.seconds = ImGui.InputInt("length, s", P.seconds)
    ImGui.SameLine()
    if ImGui.Button("Profile") then
        local ok, r = pcall(function() return Game.RG_Profile(P.delay, P.seconds) end)
        P.msg = not ok and tostring(r) or (r == "" and "" or ("not started: " .. r))
    end
    tip("Measures REDGraph's own CPU time per frame at every point it hooks - the game's work and the\n" ..
        "mods' callbacks not counted. The report goes into REDGraph's log and below.")
    if P.msg ~= "" then ImGui.TextColored(1.0, 0.45, 0.35, 1.0, P.msg) end
    local ok, s = pcall(function() return Game.RG_ProfileState() end)
    P.state = ok and s or tostring(s)
    if P.state ~= "" then ImGui.TextWrapped(P.state) end
end

-- ---- GPU time of the host's work items (core/gpu_timing.cpp has the report format) ----

local T = { at = -1, head = nil, items = {} }

local function gpu_refresh()
    local ok, s = pcall(function() return Game.RG_GpuTiming() end)
    T.head, T.items = nil, {}
    if not ok or not s then return end
    for line in s:gmatch("[^\n]+") do
        local t = {}
        for f in (line .. "\t"):gmatch("([^\t]*)\t") do t[#t + 1] = f end
        if t[1] == "gpu" then
            T.head = { frames = t[2], late = t[3], overflow = t[4], overhead = tonumber(t[5]) or -1, nodes = t[6] == "1" }
        elseif t[1] == "item" then
            T.items[#T.items + 1] = { kind = t[2], client = t[3], name = t[4], ms = tonumber(t[5]) or -1,
                                      last = tonumber(t[6]) or 0, per_frame = t[7], frames = t[8],
                                      rejected = t[9], late = t[10] }
        end
    end
    table.sort(T.items, function(a, b) return a.ms > b.ms end)
end

local function draw_item(it)
    local name = it.kind == "node" and it.name:gsub("^CRenderNode_", "") or it.name
    local text = string.format("%8.4f ms  (last %.4f, x%s per frame)  [%s] %s / %s", it.ms, it.last,
        it.per_frame, it.kind, it.client, name)
    if it.ms < 0 then ImGui.TextDisabled(text) else ImGui.Text(text) end
    if (tonumber(it.rejected) or 0) > 0 or (tonumber(it.late) or 0) > 0 then
        note("rejected " .. it.rejected .. ", late " .. it.late)
    end
end

local function draw_gpu()
    heading("GPU time")
    check("GPU timing", "Diagnostics.GpuTiming",
        "Measures the GPU time of every mod companion and pass REDGraph records on the game's command\n" ..
        "lists (GPU timestamps, read a few frames later - never waits) and marks each one as a debug\n" ..
        "event for Nsight / RenderDoc. On at the game's START it also times every node of the game's\n" ..
        "render graph - switch a mod's change off and on to see what it adds to its node.\n" ..
        "A number per item, smoothed; not an absolute truth. Live.")
    if not on("Diagnostics.GpuTiming") then return end
    local now = os.clock()
    if now - T.at > 0.5 then
        T.at = now
        gpu_refresh()
    end
    if not T.head then
        note("waiting for the first frames")
        return
    end
    note(string.format("frames read %s, late pairs %s, frames over budget %s, timing's own cost %.4f ms",
        T.head.frames, T.head.late, T.head.overflow, T.head.overhead))
    if not T.head.nodes then
        note("the game's render graph nodes are not timed: GPU timing was off when the game started")
    end
    local nodes, total = {}, 0
    for _, it in ipairs(T.items) do
        if it.kind == "node" then
            nodes[#nodes + 1] = it
            if it.ms > 0 then total = total + it.ms end
        else
            draw_item(it)
        end
    end
    if #nodes > 0 and ImGui.TreeNode(string.format("Game render graph nodes (%d, sum %.2f ms)", #nodes, total)) then
        for _, it in ipairs(nodes) do draw_item(it) end
        ImGui.TreePop()
    end
    if #T.items == 0 then note("nothing timed yet") end
end

local function draw_host()
    ImGui.Text(plugin)
    heading("Diagnostics")
    check("Detailed log", "Diagnostics.VerboseLog",
        "Writes every ray-tracing library the game builds and every shader swap into REDGraph's\n" ..
        "log, not just the first of each. Most of that happens while the game starts, so turn it on,\n" ..
        "restart the game, and attach the log to a bug report.")
    check("Render graph debug window (restart)", "Diagnostics.RenderGraphDebug",
        "A developer tool: a window with every node of the game's render graph and the shaders each\n" ..
        "one binds. Any node can be switched off live, any shader skipped - everywhere or inside one\n" ..
        "node. Takes effect from the next start. Leave it off for playing.")
    if G.head then
        ImGui.Indent()
        G.show = ImGui.Checkbox("Show the render graph window", G.show)
        tip("Hides or shows the window only - removed nodes and skipped shaders stay as they are.")
        ImGui.Unindent()
    elseif on("Diagnostics.RenderGraphDebug") then
        note("the window appears in game - after a restart if just switched on")
    end
    check("Render graph dump (restart)", "Diagnostics.RenderGraphDump",
        "Writes every distinct render graph and a census of it into render-graph/ next to REDGraph.dll.\n" ..
        "Takes effect from the next start.")
    check("DRED in crash reports (restart)", "Diagnostics.Dred",
        "Asks the driver for the GPU page-fault address and breadcrumbs, written to the log on a device\n" ..
        "removal. Costs a little GPU time. Takes effect from the next start.")
    check("BLAS census (restart)", "Diagnostics.BlasCensus",
        "Every 10 s in the log: which watched meshes are in the ray-traced scene (static / animated / absent)\n" ..
        "and what the game's BVH pool writers did. The mods using REDGraph name what is watched.\n" ..
        "Takes effect from the next start.")
    check("Frame timer", "Diagnostics.FrameTimer",
        "The frame time every 10 s in the log - for an on / off comparison of any mod's feature.")
    ImGui.Spacing()
    if ImGui.Button("Status into the log") then
        local ok, s = pcall(function() return Game.RG_Status() end)
        status = ok and s or tostring(s)
    end
    tip("REDGraph's counters and settings - also written into its log.")
    if status ~= "" then ImGui.TextWrapped(status) end
    draw_gpu()
    draw_profile()
end

-- ---- the clients tab: who registered what, what was refused (the report format: core/clients.cpp) ----

local C = { clients = {}, entries = {}, shared = {}, dropped = 0, plumbing = false, only_bad = false }

-- Registrations that only carry data for the others (buffers, views, resources by name, a node's declarations).
local kPlumbing = { ["cbuffer"] = true, ["cbuffer feed"] = true, ["view"] = true, ["device_ready"] = true,
                    ["flow"] = true, ["node use"] = true, ["node alloc"] = true }

local function short_hashes(s)
    return (s:gsub("%x+", function(h) if #h == 32 then return h:sub(1, 8) end end))
end

local function clients_refresh()
    local ok, s = pcall(function() return Game.RG_Clients() end)
    C.clients, C.entries, C.shared, C.dropped = {}, {}, {}, 0
    if not ok or not s then return end
    for line in s:gmatch("[^\n]+") do
        local t = {}
        for f in (line .. "\t"):gmatch("([^\t]*)\t") do t[#t + 1] = f end
        if t[1] == "C" then
            C.clients[#C.clients + 1] = { i = tonumber(t[2]), name = t[3], api = t[4], how = t[5],
                                          regs = tonumber(t[6]) or 0, bad = tonumber(t[7]) or 0 }
        elseif t[1] == "E" then
            C.entries[#C.entries + 1] = { client = tonumber(t[2]), kind = t[3], handle = t[4], name = t[5],
                                          target = t[6], state = t[7], live = t[8], why = t[9] }
        elseif t[1] == "X" then
            C.shared[#C.shared + 1] = { family = t[2], target = t[3], who = t[4] }
        elseif t[1] == "D" then
            C.dropped = tonumber(t[2]) or 0
        end
    end
end

local function entry_bad(e) return e.state:find("^ERROR") or e.state == "REFUSED" end

local function draw_entry(e)
    local text = "[" .. e.kind .. "] " .. (e.name ~= "" and e.name or ("#" .. e.handle))
    if e.target ~= "" then text = text .. "  ->  " .. short_hashes(e.target) end
    text = text .. "   " .. e.state
    if e.live ~= "" then text = text .. "   " .. e.live end
    if entry_bad(e) then
        ImGui.TextColored(1.0, 0.45, 0.35, 1.0, text)
        if e.why ~= "" then ImGui.TextColored(1.0, 0.45, 0.35, 1.0, "      why: " .. e.why) end
    elseif e.state == "pending" then
        ImGui.TextDisabled(text)
    else
        ImGui.Text(text)
    end
end

local function draw_clients()
    if ImGui.Button("Refresh##clients") then clients_refresh() end
    tip("Read the registrations and their counters again.")
    ImGui.SameLine()
    C.plumbing = ImGui.Checkbox("show buffers / views / declarations", C.plumbing)
    ImGui.SameLine()
    C.only_bad = ImGui.Checkbox("only refused", C.only_bad)
    if #C.clients == 0 then
        ImGui.TextWrapped("No client has reached REDGraph. A mod that uses it registers while the game starts.")
        return
    end
    for _, c in ipairs(C.clients) do
        local how = c.how == "waited" and ", loaded before REDGraph" or
            (c.how == "skipped" and ", NOT RUN - built for a newer REDGraph" or "")
        local head = c.name .. "   API " .. c.api .. how .. "   " .. c.regs .. " registrations" ..
            (c.bad > 0 and (", " .. c.bad .. " REFUSED") or "")
        if ImGui.TreeNode(head .. "##c" .. c.i) then
            for _, e in ipairs(C.entries) do
                if e.client == c.i and (C.plumbing or not kPlumbing[e.kind]) and (not C.only_bad or entry_bad(e)) then
                    draw_entry(e)
                end
            end
            ImGui.TreePop()
        end
    end
    if C.dropped > 0 then ImGui.Text(C.dropped .. " registrations not listed (the host's list is full)") end
    heading("Registers (space0 of the game's own descriptor tables)")
    local regs, order = {}, {}
    for _, e in ipairs(C.entries) do
        if e.kind == "lease" and e.target ~= "" then
            if not regs[e.target] then
                regs[e.target] = {}
                order[#order + 1] = e.target
            end
            local who = (C.clients[e.client + 1] or {}).name or "?"
            local held = e.state:find("^ERROR") and "  (not granted)" or (e.state == "REFUSED" and "  (REFUSED)" or "")
            table.insert(regs[e.target], e.name .. " - " .. who .. held)
        end
    end
    if #order == 0 then
        note("no client holds a register")
    else
        table.sort(order)
        for _, t in ipairs(order) do ImGui.Text(t .. ":   " .. table.concat(regs[t], ";   ")) end
        note("A register is shared freely by leases of DIFFERENT shaders. [RT] = staged at every ray-tracing")
        note("dispatch: one RT lease per register, a second one is refused.")
    end
    heading("Shared targets")
    if #C.shared == 0 then
        note("none - no shader, ray-tracing raygen, node or register is used by two clients")
    else
        note("the same engine shader / raygen / node / register, registered by more than one client:")
        for _, x in ipairs(C.shared) do
            ImGui.Text(x.family .. "  " .. short_hashes(x.target) .. "   " .. x.who)
        end
        note("A patch: the higher priority runs, the other shows REFUSED. A raygen variant: of those switched")
        note("on, the higher library priority runs. Wraps on one node: every before, the node, every after -")
        note("each in registration order. A node is removed by ONE registration - a second one is refused.")
    end
end

-- ---- the render graph debug window ([Diagnostics] RenderGraphDebug; the report format: core/graph_debug.hpp) ----

G = { head = nil, gen = 0, nodes = {}, kids = {}, sh = {}, rules = {}, filter = "", force = false, msg = "",
            next_poll = 0, max = -1, show = true,
            only_shaders = true, hide_structural = false, only_changed = false, kind = 0 }  -- kind 0 all 1 gfx 2 cs 3 rt

local kKindPipe = { [1] = 1, [2] = 0, [3] = 2 }  -- kind -> the row's pipe

local function row_shown(r) return G.kind == 0 or r.pipe == kKindPipe[G.kind] end

-- Per node, bottom-up (the tree is pre-order, so every child has a larger index): its rows under the kind filter, their
-- binds, whether it or anything below it changed, whether it is shown.
local function compute_vis()
    for i = G.max, 0, -1 do
        local n = G.nodes[i]
        if n then
            local rows, binds, changed = 0, 0, n.removed
            for _, r in ipairs(G.sh[i] or {}) do
                if row_shown(r) then rows, binds = rows + 1, binds + r.binds end
                if r.here or r.all then changed = true end
            end
            n.rows, n.binds = rows, binds
            local kid_vis, kid_chg = false, false
            for _, k in ipairs(G.kids[i] or {}) do
                kid_vis = kid_vis or G.nodes[k].vis
                kid_chg = kid_chg or G.nodes[k].chg
            end
            n.chg = changed or kid_chg
            local own = (not G.only_shaders or rows > 0)
                and not (G.hide_structural and n.flags:find("s", 1, true))
                and (not G.only_changed or changed)
            n.vis = own or kid_vis
        end
    end
end

local function split(line)
    local t = {}
    for f in (line .. "\t"):gmatch("([^\t]*)\t") do t[#t + 1] = f end
    return t
end

local function now()
    local ok, t = pcall(os.clock)
    return ok and t or 0
end

local function graph_refresh()
    local ok, s = pcall(function() return Game.RG_Graph() end)
    G.head = nil
    if not ok or not s or s == "" then return end
    G.nodes, G.kids, G.sh, G.rules, G.max = {}, { [-1] = {} }, {}, {}, -1
    for line in s:gmatch("[^\n]+") do
        local t = split(line)
        if t[1] == "H" then
            G.head = { sig = t[3], mode = t[4], classes = t[5], census = t[6], removed = t[7], elsewhere = t[8],
                       rules = t[9] }
            G.gen = tonumber(t[2]) or 0
            G.frames = tonumber(t[6]:match("^(%d+) frames")) or 0
        elseif t[1] == "N" then
            local n = { i = tonumber(t[2]), parent = tonumber(t[3]), depth = tonumber(t[4]), flags = t[5],
                        removed = t[6] ~= "0", by_client = t[6] == "2", name = t[7], key = t[8], rem = t[9], ran = t[10], refused = t[11],
                        owner = t[12] }
            G.nodes[n.i] = n
            G.kids[n.i] = {}
            if n.i > G.max then G.max = n.i end
            table.insert(G.kids[n.parent] or G.kids[-1], n.i)
            if n.removed then  -- a group shows "not all running" when any member is removed
                local p = n.parent
                while p and p >= 0 and G.nodes[p] do
                    G.nodes[p].some_removed = true
                    p = G.nodes[p].parent
                end
            end
        elseif t[1] == "S" then
            local r = { row = tonumber(t[2]), node = tonumber(t[3]), pipe = tonumber(t[4]), vs = t[5], ps = t[6],
                        cs = t[7], binds = tonumber(t[8]) or 0, psos = t[9], here = t[10] == "1", all = t[11] == "1",
                        sk_here = t[12], sk_all = t[13], rt = t[14] }
            G.sh[r.node] = G.sh[r.node] or {}
            table.insert(G.sh[r.node], r)
        elseif t[1] == "R" then
            table.insert(G.rules, { i = tonumber(t[2]), vs = t[3], ps = t[4], cs = t[5], node = t[6], skipped = t[7] })
        end
    end
    compute_vis()
end

local function result(r)
    G.msg = r or ""
    if G.msg:find("^stale") then G.msg = "the graph changed - refreshed, try again" end
    graph_refresh()
end

local function node_set(i, removed)
    local ok, r = pcall(function() return Game.RG_GraphNode(G.gen, i, removed, G.force) end)
    result(ok and r or tostring(r))
end

local function shader_set(row, where, skip)
    local ok, r = pcall(function() return Game.RG_GraphShader(G.gen, row, where, skip) end)
    result(ok and r or tostring(r))
end

local function has(n, f) return n.flags:find(f, 1, true) ~= nil end

local function short(h) return (h and h ~= "-") and h:sub(1, 8) or nil end

local function shader_text(r)
    if r.pipe == 2 then return "RT " .. r.rt end
    local s = {}
    if short(r.vs) then s[#s + 1] = "vs " .. short(r.vs) end
    if short(r.ps) then s[#s + 1] = "ps " .. short(r.ps) end
    if short(r.cs) then s[#s + 1] = "cs " .. short(r.cs) end
    local per = G.frames > 0 and string.format("  %.1f binds/frame", r.binds / G.frames) or ""
    return table.concat(s, " ") .. per
end

local function draw_shaders(i)
    for _, r in ipairs(G.sh[i] or {}) do
        if not row_shown(r) then
            -- hidden by the kind filter
        elseif r.pipe ~= 2 then
            if i >= 0 then
                local v, p = ImGui.Checkbox("##h" .. r.row, not r.here)
                if p then shader_set(r.row, 1, not v) end
                tip("Ticked: this shader set draws / dispatches in THIS node. Untick = skipped here only\n" ..
                    "(the engine's own path: the bind returns no PSO, the draw is dropped).")
                ImGui.SameLine()
            end
            local v, p = ImGui.Checkbox("##a" .. r.row, not r.all)
            if p then shader_set(r.row, 2, not v) end
            tip("Ticked: this shader set runs EVERYWHERE. Untick = skipped in every node.")
            ImGui.SameLine()
            local skipped = ""
            if r.here then skipped = skipped .. "  skipped here " .. r.sk_here end
            if r.all then skipped = skipped .. "  skipped " .. r.sk_all end
            ImGui.Text(shader_text(r) .. skipped)
        else
            ImGui.TextDisabled("      " .. shader_text(r))
        end
    end
end

local function draw_node(i, label_prefix)
    local n = G.nodes[i]
    local name = n.name:gsub("^CRenderNode_", "")
    local group = has(n, "g")
    local suffix = ""
    if has(n, "s") then suffix = suffix .. "  [structural]" end
    if has(n, "j") then suffix = suffix .. "  (job - cannot be removed)" end
    if has(n, "o") then
        suffix = suffix .. "  (node of " .. ((n.owner and n.owner ~= "-") and n.owner or "a client") .. ")"
    end
    if n.by_client then
        suffix = suffix .. "  REMOVED by " .. n.owner .. " (every node of its class) " .. n.rem .. " / ran " .. n.ran
    elseif n.removed then
        suffix = suffix .. "  REMOVED " .. n.rem .. " / ran " .. n.ran
    end
    if (n.rows or 0) > 0 then
        suffix = suffix .. "   [" .. n.rows .. " sh" ..
            (G.frames > 0 and string.format(", %.0f/frame", n.binds / G.frames) or "") .. "]"
    end
    if n.by_client then
        ImGui.TextDisabled("[-]")
        tip("Removed by a mod (node_remove) - only that mod can bring it back.")
    elseif group or (has(n, "w") and not has(n, "j") and not has(n, "o")) then
        local running = not (group and n.some_removed or n.removed)
        local v, p = ImGui.Checkbox("##n" .. i, running)
        if p then node_set(i, not v) end
        tip(group and "Ticked: every member runs. Untick = remove every member that can be removed." or
            "Ticked: the node runs. Untick = its work is removed live (its resource declarations stay).")
    else
        ImGui.TextDisabled("[ ]")
    end
    ImGui.SameLine()
    local kids = {}
    for _, k in ipairs(G.kids[i] or {}) do
        if G.nodes[k].vis then kids[#kids + 1] = k end
    end
    if #kids > 0 or (n.rows or 0) > 0 then
        if ImGui.TreeNode((label_prefix or "") .. name .. suffix .. "##t" .. i) then
            draw_shaders(i)
            for _, k in ipairs(kids) do draw_node(k) end
            ImGui.TreePop()
        end
    else
        ImGui.Text((label_prefix or "") .. name .. suffix)
    end
end

local function matches(i, f)
    local n = G.nodes[i]
    if n.name:lower():find(f, 1, true) then return true end
    for _, r in ipairs(G.sh[i] or {}) do
        if (r.vs .. r.ps .. r.cs .. (r.rt or "")):find(f, 1, true) then return true end
    end
    return false
end

local function draw_graph_window()
    if not G.head or not G.show then return end
    if G.head.census:find("^running") and now() > G.next_poll then
        G.next_poll = now() + 1
        graph_refresh()
        if not G.head then return end
    end
    if ImGui.Begin("Render Graph (debug)") then
        local h = G.head
        ImGui.Text("graph " .. h.sig:sub(1, 8) .. "  mode " .. h.mode .. "  classes wrapped " .. h.classes ..
            "  census: " .. h.census)
        ImGui.Text("removed here " .. h.removed .. ", wanted in other graphs " .. h.elsewhere .. ", shader rules " ..
            h.rules)
        if ImGui.Button("Refresh") then G.msg = ""; graph_refresh() end
        ImGui.SameLine()
        if ImGui.Button("New census") then pcall(function() Game.RG_GraphAction(0) end); graph_refresh() end
        tip("Count again which shaders every node binds (300 frames). Runs by itself whenever the graph changes.")
        ImGui.SameLine()
        if ImGui.Button("Reset all") then pcall(function() Game.RG_GraphAction(1) end); G.msg = ""; graph_refresh() end
        tip("Every node runs again and no shader is skipped.")
        ImGui.SameLine()
        G.force = ImGui.Checkbox("allow structural", G.force)
        tip("Structural nodes bind render targets / constants or keep the frame's books (Set*, End*,\n" ..
            "Bind*, Synchronize, Present ...). Removing one leaves the engine's state wrong for every node\n" ..
            "after it and CAN TAKE THE DRIVER DOWN. Tick only to try exactly that.")
        if G.msg ~= "" then ImGui.TextWrapped("! " .. G.msg) end
        local f = ImGui.InputText("filter (name or hash)", G.filter, 64)
        if f ~= nil then G.filter = f end
        local changed, v, p = false
        v, p = ImGui.Checkbox("only with shaders", G.only_shaders)
        if p then G.only_shaders, changed = v, true end
        tip("Hide nodes that bound no shader in the census (render-target / constant binds, CPU work,\n" ..
            "nodes idle in this render mode) and groups left empty by that.")
        ImGui.SameLine()
        v, p = ImGui.Checkbox("hide structural", G.hide_structural)
        if p then G.hide_structural, changed = v, true end
        ImGui.SameLine()
        v, p = ImGui.Checkbox("only changed", G.only_changed)
        if p then G.only_changed, changed = v, true end
        tip("Only nodes switched off and shaders skipped (and the groups they are in).")
        for k, label in ipairs({ "all", "graphics", "compute", "RT" }) do
            if k > 1 then ImGui.SameLine() end
            if ImGui.RadioButton(label .. "##kind", G.kind == k - 1) then G.kind, changed = k - 1, true end
        end
        if changed then compute_vis() end
        ImGui.Separator()
        local filter = G.filter:lower()
        if filter ~= "" then
            for i = 0, G.max do
                if G.nodes[i] and G.nodes[i].vis and matches(i, filter) then
                    local par = G.nodes[i].parent
                    draw_node(i, par >= 0 and (G.nodes[par].name:gsub("^CRenderNode_", "") .. " / ") or "")
                end
            end
        else
            for _, i in ipairs(G.kids[-1]) do
                if G.nodes[i].vis then draw_node(i) end
            end
        end
        if G.sh[-1] and #G.sh[-1] > 0 then
            if ImGui.TreeNode("(outside any node - work handed to jobs)##nojob") then
                draw_shaders(-1)
                ImGui.TreePop()
            end
        end
        if #G.rules > 0 then
            ImGui.Separator()
            ImGui.Text("Shader skip rules")
            for _, r in ipairs(G.rules) do
                if ImGui.Button("x##r" .. r.i) then
                    local ok, res = pcall(function() return Game.RG_GraphRuleOff(r.i) end)
                    result(ok and res or tostring(res))
                end
                ImGui.SameLine()
                local s = {}
                if short(r.vs) then s[#s + 1] = "vs " .. short(r.vs) end
                if short(r.ps) then s[#s + 1] = "ps " .. short(r.ps) end
                if short(r.cs) then s[#s + 1] = "cs " .. short(r.cs) end
                ImGui.Text(table.concat(s, " ") .. "  in " .. r.node .. "  skipped " .. r.skipped)
            end
        end
    end
    ImGui.End()
end

registerForEvent("onInit", function()
    refresh()
    if plugin then log(plugin) end
end)

registerForEvent("onOverlayOpen", function()
    overlay = true
    refresh()
    if plugin then
        graph_refresh()
        clients_refresh()
    end
end)
registerForEvent("onOverlayClose", function() overlay = false end)

registerForEvent("onDraw", function()
    if not overlay then return end
    if ImGui.Begin("REDGraph") then
        if not plugin then
            ImGui.TextWrapped("REDGraph's RED4ext plugin is not loaded. Install RED4ext and keep\n" ..
                "red4ext/plugins/REDGraph/ from REDGraph's archive.")
        elseif ImGui.BeginTabBar("##rgtabs") then
            if ImGui.BeginTabItem("Host") then
                draw_host()
                ImGui.EndTabItem()
            end
            if ImGui.BeginTabItem("Clients") then
                draw_clients()
                ImGui.EndTabItem()
            end
            ImGui.EndTabBar()
        end
    end
    ImGui.End()
    if plugin then draw_graph_window() end
end)
