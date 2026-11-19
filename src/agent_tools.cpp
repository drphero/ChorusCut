#include "agent_tools.h"
#include <QJsonDocument>
#include <QtGlobal>

namespace AgentTools {

QString socketName() {
    const QString custom = qEnvironmentVariable("CHORUS_AGENT_SOCKET");
    return custom.isEmpty() ? QStringLiteral("chorus-cut-agent") : custom;
}

static QJsonObject tool(const char* name, const char* title, const QString& description, QJsonObject properties = {},
                        QJsonArray required = {}, bool readOnly = true) {
    QJsonObject schema{{"type", "object"}, {"properties", properties}};
    if (!required.isEmpty()) schema["required"] = required;
    return {{"name", name}, {"title", title}, {"description", description}, {"inputSchema", schema},
            {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"destructiveHint", false}, {"openWorldHint", false}}}};
}

QJsonArray tools() {
    return {
        tool("get_guide", "Planning guide", "How to plan or revise the music video in Chorus Cut, step by step. Read it first whenever "
             "the user asks you to plan, draft or rework the video."),
        tool("get_project", "Project", "The project open in Chorus Cut: name, frame rate, length, lyric state, plan size and the planning gates. "
             "Call this first. If no project is open, ask the user to open one."),
        tool("get_song", "Song", "The song's sections in order (lyric [headings] and instrumental parts) and its sung lines with times in "
             "milliseconds and project frames, review flags and who sings each line. Set include_words for per-word times.",
             {{"include_words", QJsonObject{{"type", "boolean"}, {"description", "Include each word's start and end."}}}}),
        tool("get_plan", "Plan", "Everything planned so far: concept and look, characters with looks and reference pictures, locations, "
             "the arc for each section, and shots with their fields. Each field's provenance says who wrote it (user or agent) and whether "
             "it is locked."),
        tool("get_checks", "Checks", "The plan checks and gates: errors (wrong), to-dos (unfinished) and advice, each with the shots and "
             "times it concerns. Errors and to-dos hold back batch rendering."),
        tool("get_schema", "Schema", "How to write the plan: every command (op), the fields of each kind with their allowed values, the "
             "required shot fields, route length limits in frames, and the conventions for times and references."),
        tool("view_reference", "View reference picture", "Shows one reference picture (by id from get_plan), scaled down, so you can "
             "describe a character or place from it.",
             {{"id", QJsonObject{{"type", "integer"}, {"description", "Reference id from get_plan."}}}}, {"id"}),
        tool("apply_plan", "Apply plan changes",
             "Applies plan commands as one pass: all of them or none, and one undo step the user sees as \"Agent: <label>\". "
             "Fields the user wrote are kept and locked fields are never touched; anything skipped is listed. "
             "Commands are {op, ...} objects described by get_schema. A command may set \"ref\": \"name\" and later commands in the "
             "same pass can write \"$name\" for the id it created. Returns the ids created, what was skipped and the checks afterwards. "
             "Prefer several focused passes (concept; cast and locations; arc and singers; shots) so the user can undo each.",
             {{"label", QJsonObject{{"type", "string"}, {"description", "Short name for this pass, e.g. \"Draft shot list\"."}}},
              {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"agent", "fill", "replace"}},
                                   {"description", "agent (default): write empty fields and fields an agent wrote; fill: only empty "
                                                   "fields; replace: also the user's unlocked fields. Use replace only when the user asks."}}},
              {"commands", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}},
                                       {"description", "Plan commands, e.g. {\"op\":\"set\",\"kind\":\"concept\",\"fields\":{\"logline\":\"…\"}}."}}}},
             {"label", "commands"}, false),
    };
}

QString instructions() {
    return QStringLiteral(
        "Chorus Cut is a music-video editor. You help the user plan the video for the song open in the app: read it with get_project, "
        "get_song, get_plan, get_checks and get_schema, look at reference pictures with view_reference, and write with apply_plan. "
        "The user watches the plan change live in the app; each apply_plan call is one undo step for them, and fields they wrote or "
        "locked are protected. Shot times are project frames (start inclusive, end exclusive); lyric times are milliseconds. "
        "You cannot hear the song: use the sections, line timings and the user's direction. Nothing is generated here yet; you are "
        "planning what will be generated. The concept's brief is the user's own direction; agents may add to it but never rewrite it. "
        "When the user asks you to plan or rework the video, call get_guide first and follow it: it starts with a short intake "
        "the user can skip, and pauses after the concept.");
}

QString guide() {
    return QStringLiteral(R"(Plan the music video for the song open in Chorus Cut, top-down, through the Chorus Cut tools. The user decides how much to talk first; you never skip their part silently.

1. Read: get_project, get_song, get_plan, get_checks and get_schema. The concept's "brief" is the user's own direction: follow it. Respect everything the user has already written; agent mode never overwrites it, and do not use mode "replace" unless they ask.

2. Intake. Work out what the brief and the user's request already settle among: structure (performance, story or a mix), who performs and what they look like, format (aspect ratio), story or look ideas, and anything to avoid. If something important is open, ask once, before writing anything: say in a sentence or two what you read, then ask at most five short questions, each with two to four concrete options and "you choose". If your client has a multiple-choice question tool, use it. Always include the option "Just draft it all, I'll react". Skip the intake when the brief or the request already answers these, or when the user says to just draft it.
   Record what the user told you: add it to the end of the concept's brief (set kind "concept", fields {"brief": <the existing brief, then a short list of their answers in their own words>}). You can only add to the brief, never rewrite it, and only what the user said belongs there; your own inventions go in the concept fields.

3. Concept pass: kind of video, logline, treatment, aspect ratio, visual style, palette, lighting, era, lens feel, grade, and rules (what must or must never appear). Then tell the user the structure you chose and why in two or three sentences, and pause: ask them to confirm or adjust the concept before you write cast and shots, since everything after depends on it. Continue without pausing only if they said to go all the way.

4. Cast and locations pass: characters (role singer for whoever sings), concrete identity descriptions (face, hair, build, age), named looks per character, and locations with time of day and lighting. If they have reference pictures, view them and describe what you see.
5. Arc and singers pass: one arc entry per section in order (kind "arc", id = section ordinal, include "section" with its label), with what happens, the emotional beat and cutting density. Then set the singer of every sung line with op "singers".
6. Shots pass: cover every frame of the song. Cut on line boundaries using the frame times from get_song; keep each shot within its route's maxFrames; lip-sync shots only where words are sung, with the line's singer in frame and route "h3-lipsync"; vary framing and movement; put shots on V1 and V2 freely. Fill the required core of every shot: type, purpose, who, where, framing, move, route. A deliberate gap needs only type "gap" and a purpose (the reason).
7. Check: call get_checks and fix what you can. Then tell the user what is left for them, such as reference pictures to add or approve, and lines to review.

Keep each pass coherent and labelled so the user can undo it on its own. Use ids from get_plan, or "ref" and "$name" within one pass.)");
}

QJsonArray prompts() {
    return {QJsonObject{{"name", "plan"}, {"title", "Plan the music video"},
                        {"description", "Plan the open song's video top-down in Chorus Cut: concept, cast, locations, arc, singers and shots."},
                        {"arguments", QJsonArray{QJsonObject{{"name", "direction"}, {"required", false},
                                                             {"description", "Anything the user already wants: story, style, cast, references."}}}}}};
}

QJsonArray promptMessages(const QString& name, const QJsonObject& arguments) {
    if (name != "plan") return {};
    QString text = guide();
    const QString direction = arguments.value("direction").toString().trimmed();
    if (!direction.isEmpty()) text += "\n\nThe user's direction (record it in the brief): " + direction;
    return {QJsonObject{{"role", "user"}, {"content", QJsonObject{{"type", "text"}, {"text", text}}}}};
}
}
