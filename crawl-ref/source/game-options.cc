/*
 *  @file
 *  @brief Global game options controlled by the rcfile.
 */

#include "AppHdr.h"

#include "game-options.h"
#include "initfile.h"
#include "lookup-help.h"
#include "options.h"
#include "menu.h"
#include "message.h"
#include "misc.h"
#include "prompt.h"
#include "tiles-build-specific.h"

// Return a list of the valid field values for _curses_attribute(), excluding
// hi: and hilite: values, which are all duplicates.
// This is called after initialisation to ensure that colour_to_str() works.
static map<unsigned, string>& _curses_attribute_map()
{
    static map<unsigned, string> list;
    if (list.empty())
    {
        // This is the same list as in _curses_attribute.
        list =
        {
            {CHATTR_NORMAL, "none"}, {CHATTR_STANDOUT, "standout"},
            {CHATTR_BOLD, "bold"}, {CHATTR_BLINK, "blink"},
            {CHATTR_UNDERLINE, "underline"}, {CHATTR_REVERSE, "reverse"},
            {CHATTR_DIM, "dim"}
        };
        for (colour_t i = COLOUR_UNDEF; i < NUM_TERM_COLOURS; ++i)
        {
            int idx = CHATTR_HILITE | i << 8;
            list[idx] = string("highlight:")+colour_to_str(i);
        }
    }
    return list;
}

static unsigned _curses_attribute(const string &field, string &error)
{
    if (field == "standout")               // probably reverses
        return CHATTR_STANDOUT;
    if (field == "bold")              // probably brightens fg
        return CHATTR_BOLD;
    if (field == "blink")             // probably brightens bg
        return CHATTR_BLINK;
    if (field == "underline")
        return CHATTR_UNDERLINE;
    if (field == "reverse")
        return CHATTR_REVERSE;
    if (field == "dim")
        return CHATTR_DIM;
    if (starts_with(field, "hi:")
        || starts_with(field, "hilite:")
        || starts_with(field, "highlight:"))
    {
        const int col = field.find(":");
        const int colour = str_to_colour(field.substr(col + 1));
        if (colour != -1)
            return CHATTR_HILITE | (colour << 8);

        error = make_stringf("Bad highlight string -- %s",
                             field.c_str());
    }
    else if (field != "none")
        error = make_stringf("Bad colour -- %s", field.c_str());
    return CHATTR_NORMAL;
}

/**
 * Read a maybe bool field. Accepts anything for the third value.
 */
maybe_bool read_maybe_bool(const string &field)
{
    // TODO: check for "maybe" explicitly or something?
    if (field == "true" || field == "1" || field == "yes")
        return MB_TRUE;

    if (field == "false" || field == "0" || field == "no")
        return MB_FALSE;

    return MB_MAYBE;
}

bool read_bool(const string &field, bool def_value)
{
    const maybe_bool result = read_maybe_bool(field);
    if (result != MB_MAYBE)
        return tobool(result, false);

    Options.report_error("Bad boolean: %s (should be true or false)", field.c_str());
    return def_value;
}

class option_chooser : public Menu
{
public:
    option_chooser(int _flags, GameOption *_caller)
        : Menu(_flags), caller(_caller) { }
    int pre_process(int key) override
    {
        if ('?' == key)
        {
            caller->show_help();
            return static_cast<int>(CK_NO_KEY);
        }
        else
            return key;
    }
private:
    GameOption *caller;
};

static string _make_clgo_title(string val)
{
    string item_title = replace_all(val, "<", "<<");
    if (' ' == val[0])
        return "<h>REMOVE</h>"+item_title;
    return item_title;
}

// Every MenuEntry must be selectable, and must be backed by an entry in
// CustomListGameOption::value.
class CLGO_Menu : public Menu
{
public:
    bool changed = false;

    CLGO_Menu(int _flags, GameOption *_caller, vector<string> &_list,
              bool _use_minus)
        : Menu(_flags), caller(_caller), list(_list), use_minus(_use_minus)
        { reset_items(); }

    int pre_process(int key) override
    {
        if ('(' == key && last_hovered) // move item up the list
        {
            int other = last_hovered-1;
            swap(list[last_hovered], list[other]);
            reset_items();
            changed = true;
            key = CK_UP;
        }
        else if (')' == key && last_hovered != (int)items.size()-1) // down
        {
            int other = last_hovered+1;
            swap(list[last_hovered], list[other]);
            reset_items();
            changed = true;
            key = CK_DOWN;
        }
        else if ('+' == key) // add an item
        {
            list.insert(list.begin()+last_hovered, string());
            reset_items();
            changed = true;
            key = CK_ENTER;
        }
        else if ('-' == key && use_minus) // add a REMOVE item
        {
            list.insert(list.begin()+last_hovered, " ");
            reset_items();
            changed = true;
            key = CK_ENTER;
        }
        else if (CK_BKSP == key || CK_DELETE == key)
            key = CK_ENTER; // sorry, no 1 key delete without an undelete.
        else if ('?' == key)
        {
            caller->show_help();
            key = CK_NO_KEY;
        }
        return key;
    }

    void reset_items()
    {
        clear();

        for (unsigned i = 0, size = list.size(); i < size; ++i)
        {
            const char letter = index_to_letter(i % 52);
            string item_title = _make_clgo_title(list[i]);
            MenuEntry* entry = new EGP_MenuEntry(item_title, MEL_ITEM, 1,
                                                 letter, 5);
            entry->data = &list[i];
            add_entry(entry);
        }
        if (!list.size()) // The user can't add to a blank list.
        {
            add_entry(new MenuEntry(dummy_string, 0,
                                    [](const MenuEntry&){return true;}));
        }
        update_menu(true);
    }

private:
    const string dummy_string = "<h>Press + to set this option.</h>";
    GameOption *caller;
    vector<string> &list;
    bool use_minus;
};

/// Ask the user to choose between a set of options.
///
/// @param[in] prompt Text to put above the options. Typically a question.
/// @param[in] options List of options (as strings) to choose between.
/// @param[in] def_value Value of default option (as string).
/// @returns The selected option if something was selected, or "" if the prompt
///          was cancelled.
static string _choose_one_from_list(const string prompt, GameOption *caller,
                                    const vector<string> options,
                                    const string def_value)
{
    // The caller should remove any user-provided formatting.
    option_chooser menu(MF_SINGLESELECT | MF_NO_SELECT_QTY | MF_ARROWS_SELECT
                       | MF_ALLOW_FORMATTING | MF_INIT_HOVER, caller);

    menu.set_title(new MenuEntry(prompt, MEL_TITLE));

    for (unsigned i = 0, size = options.size(); i < size; ++i)
    {
        const char letter = index_to_letter(i % 52);
        MenuEntry* me = new MenuEntry(options[i], MEL_ITEM, 1, letter);
        menu.add_entry(me);
    }

    auto it = find(options.begin(), options.end(), def_value);
    ASSERT(options.end() != it);
    menu.set_hovered(distance(options.begin(), it));

    vector<MenuEntry*> sel = menu.show();
    if (sel.empty())
        return "";
    else
        return sel[0]->text;
}

/// Ask the user to choose between a set of options.
///
/// @param[in,out] caller  Option to edit. Reads the name and current value.
///                        Writes the new value.
/// @param[in]     choices Options to choose between.
/// @returns       True if something is chosen, false otherwise.
bool choose_option_from_UI(GameOption *caller, vector<string> choices)
{
    string prompt = "Select a value for "+caller->name()+" (? for help):";
    string selected = _choose_one_from_list(prompt, caller, choices,
                                            caller->str());
    if (!selected.empty())
        caller->loadFromString(selected, RCFILE_LINE_EQUALS);
    return !selected.empty();
}

/// Ask the user to edit a game option using a list of strings.
///
/// @param[in,out] caller Option to edit. Reads various things.
///                       Writes the updated values.
/// @param[in,out] value  value of caller, as a list of strings.
/// @param[in]     use_minus If true, - adds a request to remove something
///                          (which loadFromString() evaluates).
///                          If false, - does nothing.
/// @returns       True if something has been changed, false otherwise.
bool load_list_from_UI(GameOption *caller, vector<string> &value,
                       bool use_minus)
{
    string prompt = string("Select a line to edit for \"")+caller->name()+"\":";

    CLGO_Menu menu(MF_SINGLESELECT | MF_NO_SELECT_QTY | MF_ARROWS_SELECT
                   | MF_ALLOW_FORMATTING | MF_INIT_HOVER,
                   caller, value, use_minus);
    menu.set_title(new MenuEntry(prompt, MEL_TITLE));
    const string rem = use_minus ? " [<w>-</w>] add a REMOVE line" : "";
    const string more = "<lightgrey>[<w>(</w>] move up [<w>)</w>] move down"
                          " [<w>+</w>] add an ADD line"+rem
                        + " [<w>?</w>] help</lightgrey>";
    menu.set_more(formatted_string::parse_string(more));

    menu.on_single_selection = [caller, &menu](const MenuEntry &entry)
    {
        auto *option_text = static_cast<string*>(entry.data);
        unsigned remove = ' ' == option_text->c_str()[0] ? 1 : 0;
        const string verb = remove ? "remove" : "add";
        char select[1024];
        string old = option_text->substr(remove), last = old;
        while (1)
        {
            if (msgwin_get_line("Enter a value to "+verb+".",
                                select, sizeof(select), nullptr, last))
            {
                if (!old.empty() && remove)
                    *option_text = " "+old;
                else
                    *option_text = old;
                caller->loadFromString(caller->str(), RCFILE_LINE_EQUALS);
                if (old.empty())
                    menu.reset_items();
                return true;
            }
            *option_text = trimmed_string(select);
            bool blank = option_text->empty();
            if (remove && !blank)
                option_text->insert(0, " ");
            string error
                = caller->loadFromString(caller->str(), RCFILE_LINE_EQUALS);
            if (error.empty())
            {
                auto &text = const_cast<MenuEntry*>(&entry)->text;
                if (blank)
                    menu.reset_items();
                else
                    text = _make_clgo_title(*option_text);
                menu.changed = true;
                return true;
            }
            show_type_response(error);
            last = select;
        }
    };

    vector<MenuEntry*> sel = menu.show();
    return menu.changed;
}

/// Ask the user to edit a game option using a text box.
///
/// @param[in,out] caller Option to edit. Reads the name and current value.
///                       Writes the new value.
/// @returns       True if something is chosen, false otherwise.
bool load_string_from_UI(GameOption *caller)
{
    string prompt = string("Enter a value for ")+caller->name()+":";
    /// XXX - shouldn't use a fixed length string here.
    char select[1024] = "";
    string old = caller->str();
    while (1)
    {
        if (msgwin_get_line(prompt, select, sizeof(select), nullptr, old))
        {
            caller->loadFromString(caller->str(), RCFILE_LINE_EQUALS);
            return false;
        }
        string error = caller->loadFromString(select, RCFILE_LINE_EQUALS);
        if (error.empty())
            return true;
        show_type_response(error);
        old = select;
    }
}

void GameOption::set_help(int _file, int _line)
{
    help_file = _file;
    help_line = _line;
}

void GameOption::set_help(GameOption *other)
{
    set_help(other->help_file, other->help_line);
}

string GameOption::loadFromString(const string &, rc_line_type ltyp)
{
    loaded = true;
    if (parent)
        return parent->loadFromString("", ltyp);
    return "";
}

string BoolGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    string error;
    const maybe_bool result = read_maybe_bool(field);
    if (result == MB_MAYBE)
    {
        return make_stringf("Bad %s value: %s (should be true or false)",
                            name().c_str(), field.c_str());
    }

    value = tobool(result, false);
    return GameOption::loadFromString(field, ltyp);
}

bool BoolGameOption::load_from_UI()
{
    vector<string> choices = {"false", "true"};
    return choose_option_from_UI(this, choices);
}

string ColourGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    const int col = str_to_colour(field, -1, true, elemental);
    if (col == -1)
        return make_stringf("Bad %s -- %s\n", name().c_str(), field.c_str());

    value = col;
    return GameOption::loadFromString(field, ltyp);
}

const string ColourGameOption::str() const
{
    ASSERT(!elemental); // XXX - not handled, as no option uses it.
    return colour_to_str(value);
}

bool ColourGameOption::load_from_UI()
{
    vector<string> choices;
    for (colour_t i = COLOUR_UNDEF; i < NUM_TERM_COLOURS; ++i)
        choices.emplace_back(colour_to_str(i));
    return choose_option_from_UI(this, choices);
}

string CursesGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    string error;
    const unsigned result = _curses_attribute(field, error);
    if (!error.empty())
        return make_stringf("%s (for %s)", error.c_str(), name().c_str());

    value = result;
    return GameOption::loadFromString(field, ltyp);
}

const string CursesGameOption::str() const
{
    const auto x = _curses_attribute_map().find(value);
    if (x == _curses_attribute_map().end())
        die("Invalid value for %s: %d", name().c_str(), value);
    return x->second;
}

bool CursesGameOption::load_from_UI()
{
    vector<string> choices;
    for (auto &x : _curses_attribute_map()) // Add the names in numerical order.
        choices.emplace_back(x.second);
    return choose_option_from_UI(this, choices);
}

#ifdef USE_TILE
TileColGameOption::TileColGameOption(VColour &val, vector<string> _names,
                                     string _default)
        : GameOption(_names), value(val),
          default_value(str_to_tile_colour(_default)) { }

string TileColGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    if (!str_to_tile_colour(value, field))
        return make_stringf("Bad %s -- %s\n", name().c_str(), field.c_str());
    return GameOption::loadFromString(field, ltyp);
}

const string TileColGameOption::str() const
{
    return make_stringf("#%02x%02x%02x", value.r, value.g, value.b);
}
#endif

string IntGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    int val = default_value;
    if (!parse_int(field.c_str(), val))
        return make_stringf("Bad %s: \"%s\"", name().c_str(), field.c_str());
    if (val < min_value)
        return make_stringf("Bad %s: %d should be >= %d", name().c_str(), val, min_value);
    if (val > max_value)
        return make_stringf("Bad %s: %d should be <<= %d", name().c_str(), val, max_value);
    value = val;
    return GameOption::loadFromString(field, ltyp);
}

const string IntGameOption::str() const
{
    return to_string(value);
}

string StringGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    value = field;
    return GameOption::loadFromString(field, ltyp);
}

const string StringGameOption::str() const
{
    return value;
}

// set() should either return an error string and change nothing, or implement
// the option change and return "".
string CustomStringGameOption::loadFromString(const string &field,
                                              rc_line_type ltyp)
{
    string new_value = field;
    const string error = set(caller, new_value);
    if (!error.empty())
        return name()+": "+error;
    value = new_value;
    return GameOption::loadFromString(field, ltyp);
}

string ColourThresholdOption::loadFromString(const string &field,
                                             rc_line_type ltyp)
{
    string error;
    const colour_thresholds result = parse_colour_thresholds(field, &error);
    if (!error.empty())
        return error;

    switch (ltyp)
    {
        case RCFILE_LINE_EQUALS:
            value = result;
            break;
        case RCFILE_LINE_PLUS:
        case RCFILE_LINE_CARET:
            value.insert(value.end(), result.begin(), result.end());
            stable_sort(value.begin(), value.end(), ordering_function);
            break;
        case RCFILE_LINE_MINUS:
            for (pair<int, int> entry : result)
                remove_matching(value, entry);
            break;
        default:
            die("Unknown rc line type for %s: %d!", name().c_str(), ltyp);
    }
    return GameOption::loadFromString(field, ltyp);
}

colour_thresholds
    ColourThresholdOption::parse_colour_thresholds(const string &field,
                                                   string* error) const
{
    colour_thresholds result;
    for (string pair_str : split_string(",", field))
    {
        const vector<string> insplit = split_string(":", pair_str);

        if (insplit.size() != 2)
        {
            const string failure = make_stringf("Bad %s pair: '%s'",
                                                name().c_str(),
                                                pair_str.c_str());
            if (!error)
                die("%s", failure.c_str());
            *error = failure;
            break;
        }

        const int threshold = atoi(insplit[0].c_str());

        const string colstr = insplit[1];
        const int scolour = str_to_colour(colstr, -1, true, false);
        if (scolour <= 0)
        {
            const string failure = make_stringf("Bad %s: '%s'",
                                                name().c_str(),
                                                colstr.c_str());
            if (!error)
                die("%s", failure.c_str());
            *error = failure;
            break;
        }

        result.push_back({threshold, scolour});
    }
    stable_sort(result.begin(), result.end(), ordering_function);
    return result;
}

const string ColourThresholdOption::str() const
{
    auto f = [] (const colour_threshold &s)
    {
        return make_stringf("%d:%s", s.first, colour_to_str(s.second).c_str());
    };
    return comma_separated_fn(value.begin(), value.end(), f, ", ");
}

// Load a string using the provided rc_line_type. This is called internally.
string CustomListGameOption::loadFromString_real(const string &field,
                                                 rc_line_type ltyp, bool trim)
{
    vector<string> new_value, new_entries;
    if (ltyp != RCFILE_LINE_EQUALS)
        new_value = value;

    for (const auto &part : split_string(separator, field, trim))
    {
        if (part.empty())
            continue;

        if (ltyp == RCFILE_LINE_MINUS)
            new_entries.emplace_back(" "+part);
        else
            new_entries.emplace_back(part);
    }
    merge_lists(new_value, new_entries, ltyp == RCFILE_LINE_CARET);
    string error = set(caller, new_value);
    if (error.empty())
        error = GameOption::loadFromString(field, ltyp);
    if (error.empty())
        value = new_value;
    return error;
}

// Load a string, interpreting the rc_line_type according to the rules for this
// option. This is used when loading options from an external file.
string CustomListGameOption::loadFromString(const string &field,
                                            rc_line_type ltyp)
{
    convert_ltyp(ltyp);
    return loadFromString_real(field, ltyp, true);
}

bool CustomListGameOption::load_from_UI()
{
    string prompt = string("Select a line to edit for \"")+name()+"\":";

    CLGO_Menu menu(MF_SINGLESELECT | MF_NO_SELECT_QTY | MF_ARROWS_SELECT
                   | MF_ALLOW_FORMATTING | MF_INIT_HOVER,
                   this, value, use_minus);
    menu.set_title(new MenuEntry(prompt, MEL_TITLE));
    const string rem = use_minus ? "  [<w>-</w>] add a REMOVE line" : "";
    const string more = "<lightgrey>[<w>(</w>] move up  [<w>)</w>] move down"
                          "  [<w>+</w>] add an ADD line"+rem
                        + "  [<w>?</w>] help</lightgrey>";
    menu.set_more(formatted_string::parse_string(more));

    menu.on_single_selection = [this, &menu](const MenuEntry &entry)
    {
        auto *option_text = static_cast<string*>(entry.data);
        unsigned remove = ' ' == option_text->c_str()[0] ? 1 : 0;
        const string verb = remove ? "remove" : "add";
        char select[1024];
        string old = option_text->substr(remove), last = old;
        while (1)
        {
            if (msgwin_get_line("Enter a value to "+verb+".",
                                select, sizeof(select), nullptr, last))
            {
                if (!old.empty() && remove)
                    *option_text = " "+old;
                else
                    *option_text = old;
                this->loadFromString_real(this->str(), RCFILE_LINE_EQUALS);
                if (old.empty())
                    menu.reset_items();
                return true;
            }
            *option_text = trimmed_string(select);
            bool blank = option_text->empty();
            if (remove && !blank)
                option_text->insert(0, " ");
            string error
                = this->loadFromString_real(this->str(), RCFILE_LINE_EQUALS);
            if (error.empty())
            {
                auto &text = const_cast<MenuEntry*>(&entry)->text;
                if (blank)
                    menu.reset_items();
                else
                    text = _make_clgo_title(*option_text);
                menu.changed = true;
                return true;
            }
            show_type_response(error);
            last = select;
        }
    };

    vector<MenuEntry*> sel = menu.show();
    return menu.changed;
}

// Don't add REMOVE lines if loadFromString() doesn't take -= lines as they are.
void CustomListGameOption::set_minus()
{
    auto r = RCFILE_LINE_MINUS;
    convert_ltyp(r);
    use_minus = RCFILE_LINE_MINUS == r;
}

// Join the non-false options together
const string MenuGameOption::str() const
{
    string out;
    for (const auto g : children)
    {
        string gname = g->name(), myname = name(), val = g->str();
        if (starts_with(gname, myname))
            gname.erase(0, 1+myname.length()); // assume a . or _ comes next.
        if ("true" == val)
            out += ","+gname;
        else if (!val.empty() && "false" != val)
            out += ","+gname+":"+val;
    }
    if (out.empty())
        return "--->";
    return out.substr(1);
}

string MenuGameOption::loadFromString(const string &field, rc_line_type ltyp)
{
    vector<string> errors;
    for (const auto &part : split_string(",", field))
    {
        auto insplit = split_string(":", part, true, false, 2);
        if (insplit.size() < 2)
            errors.push_back("Missing :. in '"+part+"'.");
        else
            Options.read_option_line(name()+"."+insplit[0]+"="+insplit[1]);
    }
    auto tostring = [] (const string &s) { return s; };
    auto filter = [] (const string &s) { return !s.empty(); };
    string msg = comma_separated_fn(errors.begin(), errors.end(), tostring,
                                    " and ", ", ", filter);
    if (!msg.empty())
        return msg;
    else
        return GameOption::loadFromString(field, ltyp);
}

bool MenuGameOption::load_from_UI()
{
    return edit_game_prefs(this);
}

void MenuGameOption::add_child(GameOption *g)
{
    g->parent = this;
    children_w.emplace_back(g);
}
