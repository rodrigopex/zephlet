# Objectives

> **Stale (pre-v0.3).** The plan below reflects the Invoke/Report model.
> v0.3 replaced it with the envelope + rpc/events channel split.
> A fresh presentation plan belongs in a follow-up task.

- Create presentation about Zephlet framework
- Demonstrate with `ports_adapters_zbus` example
- Use typst with `main.typ` as base
- Use codly package for code blocks
- Technical language + visual aids (diagrams/images)

# Content Outline

1. **Intro**: Framework overview (Ports & Adapters on Zephyr RTOS via zbus)
2. **Architecture**: Two-channel pattern (invoke/report), loose coupling
3. **Lifecycle**: Init, start/stop, request-response with context, async events
4. **Structure**: Source vs generated files, proto → interface → impl
5. **Code Generation**: `west zephlet new` workflow, auto-regen on proto changes
6. **Adapters**: Composition via channel bridging, `west zephlet new-adapter`
7. **Demo**: Live example from ports_adapters_zbus
8. **Conclusion**: Key benefits (loose coupling, type safety, composability)

# Code Examples

- Complete zephlet proto (MsgZephlet with Invoke/Report oneofs)
- `west zephlet new` command flow + generated files
- Zephlet .c implementation (API funcs with context, report helpers)
- Adapter bridging pattern (ZBUS_ASYNC_LISTENER)
- Two-channel usage (inline invoke → API → report with context)
- Extract from tick/ui zephlets in ports_adapters_zbus

# Diagrams

- Two-channel architecture (chan_X_invoke ↔ zephlet ↔ chan_X_report)
- Data flow: inline func → invoke chan → dispatcher → API → report
- Zephlet file structure (VCS: proto/c vs Build: _interface.h/c, .pb.h/c)
- Adapter composition (origin report → listener → dest invoke)
- Build system flow (proto → CMake → codegen → nanopb)
- Request-response context pattern (correlation_id flow)

# Implementation

1. Add justfile (PDF generation + browser preview automation)
2. Extract code samples from ports_adapters_zbus example
3. Create architecture diagrams (mermaid → export, or manual)
4. Set up codly package with proper syntax highlighting
5. Structure slides per outline above
6. Replace generic Zephyr content in main.typ with Zephlet-specific content

# Testing/Validation

- Build PDF via justfile, verify generation
- Check code syntax highlighting renders correctly
- Review technical accuracy against CLAUDE.md
- Test presentation flow and timing
- Confirm all changes before commit (per user CLAUDE.md)

# Success Criteria

- Clearly explains Ports & Adapters pattern on Zephyr
- Demonstrates loose coupling via zbus channels
- Shows practical code generation workflow (west commands)
- Highlights framework differentiators (type safety, composability, auto-discovery)
- Audience can understand how to create zephlets and adapters

# Unresolved Questions

- Target audience level (Zephyr beginners vs experienced)?
- Presentation duration/depth?
- Which specific zephlets from ports_adapters_zbus to showcase (tick, ui, both)?
- Include live demo or just code samples?
- ~~Diagram format preference (mermaid, draw.io, manual)?~~ ✅ Resolved: Using Fletcher

---

# Improvements - Fletcher Diagrams (2026-02-06)

## Objectives
- Replace ASCII art with professional Fletcher-based diagrams
- Improve code block readability by adjusting text sizes
- Ensure all content fits properly on slides

## Changes Made

### 1. Added Fletcher Package
- Added import: `#import "@preview/fletcher:0.5.8": diagram, node, edge`
- Enables professional diagram creation with precise layout control

### 2. Replaced ASCII Diagram (Two-Channel Pattern slide)
**Before:** ASCII art showing Caller → Zephlet → Observer flow (overlapping text)
**After:** Clean Fletcher diagram with:
- Three rectangular nodes with proper spacing (3cm, 2cm)
- Clear labeled edges: "invoke", "status" (with bend), "events (async)" (dashed)
- Proper vertical spacing to separate from surrounding text
- No overlapping elements

### 3. Adjusted Code Block Text Sizes
Changed from 9pt to 10pt for better readability while maintaining fit:
- Tick Zephlet Implementation (slide 31) - 22 lines
- Composition via Bridging (slide 26) - 17 lines
- Generated API Structure (slide 23) - 18 lines
- Context-Aware Adapters (slides 28-29)

## Results
✅ Professional-looking Fletcher diagram replaces messy ASCII art
✅ All code blocks readable and properly sized
✅ Clean visual hierarchy and spacing
✅ No content overflow or layout issues
✅ Presentation builds without errors (316KB PDF)

## Testing
- Built with `typst compile main.typ` - success
- Visual inspection confirmed proper rendering
- Code syntax highlighting preserved with codly
- All slides fit content within boundaries
