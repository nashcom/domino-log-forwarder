
/* A label for the metrics of otelfwd (the Prometheus text format). Two instances of otelfwd write the same metric names: with a
   label which says which instance a line comes from, a collector which reads both files can tell them apart.

   Pure functions, no state: the unit test (otelfwd_unit_test.cpp) covers them without a file. */

#pragma once

#include <string>


/* A label value is written in double quotes. The Prometheus text format needs a backslash, a double quote and a new line escaped */
inline std::string PromEscapeLabelValue (const std::string& Value)
{
    std::string Result;

    Result.reserve (Value.size());

    for (char c : Value)
    {
        if ('\\' == c)
            Result += "\\\\";
        else if ('"' == c)
            Result += "\\\"";
        else if ('\n' == c)
            Result += "\\n";
        else
            Result += c;
    }

    return Result;
}


/* Adds one label to the name of a metric, in front of the labels it already has. Name is the whole metric name as it is written:
   "otelfwd_health" becomes otelfwd_health{label="value"}, and otelfwd_push_total{result="success"} becomes
   otelfwd_push_total{label="value",result="success"}. Without a label name or without a value the name is returned as it was */
inline std::string PromAddLabel (const std::string& Name, const std::string& Label, const std::string& Value)
{
    if (Label.empty() || Value.empty())
        return Name;

    std::string NewLabel = Label + "=\"" + PromEscapeLabelValue (Value) + "\"";
    size_t      Brace    = Name.find ('{');

    if (std::string::npos == Brace)
        return Name + "{" + NewLabel + "}";

    std::string Base = Name.substr (0, Brace);
    std::string Rest = Name.substr (Brace + 1);      /* the labels which are there, and the closing brace */

    if (Rest.empty())
        return Base + "{" + NewLabel + "}";

    if ('}' == Rest[0])
        return Base + "{" + NewLabel + Rest;

    return Base + "{" + NewLabel + "," + Rest;
}


/* The other direction: removes one label, with its value, from the name of a metric as it is written in the file. A reader which
   looks a metric up by its name and its labels (the load test does) takes the instance label away first, and then finds the metric
   with or without it. A name without the label is returned as it was. The braces go too if the label was the only one */
inline std::string PromRemoveLabel (const std::string& Name, const std::string& Label)
{
    size_t Brace = Name.find ('{');

    if ( (std::string::npos == Brace) || Label.empty() )
        return Name;

    std::string Prefix = Label + "=\"";
    size_t      Start  = Name.find (Prefix, Brace + 1);

    if (std::string::npos == Start)
        return Name;

    /* It is a label of its own, not the end of the name of another one */
    char Before = Name[Start - 1];

    if ( ('{' != Before) && (',' != Before) )
        return Name;

    /* The end of the value: the closing double quote, an escaped one does not count */
    size_t i = Start + Prefix.size();

    while ( (i < Name.size()) && ('"' != Name[i]) )
    {
        if ('\\' == Name[i])
            i++;

        i++;
    }

    if (i >= Name.size())
        return Name;

    size_t End = i + 1;                                  /* just after the closing double quote */

    if ( (End < Name.size()) && (',' == Name[End]) )
        return Name.substr (0, Start) + Name.substr (End + 1);

    if (',' == Before)
        return Name.substr (0, Start - 1) + Name.substr (End);

    if ( (End < Name.size()) && ('}' == Name[End]) )
        return Name.substr (0, Brace) + Name.substr (End + 1);

    return Name.substr (0, Start) + Name.substr (End);
}
