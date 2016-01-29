// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/** tokensplit.cc
    Mathieu Marquis Bolduc, November 24, 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    Function to parse strings for tokens and insert separators
*/

#include "tokensplit.h"
#include "mldb/server/analytics.h"
#include "mldb/server/mldb_server.h"
#include "mldb/core/dataset.h"
#include "mldb/server/dataset_context.h"
#include "mldb/server/function_collection.h"
#include "types/structure_description.h"
#include "mldb/types/any_impl.h"

using namespace std;


namespace Datacratic {
namespace MLDB {

DEFINE_STRUCTURE_DESCRIPTION(TokenSplitConfig);

TokenSplitConfigDescription::
TokenSplitConfigDescription()
{
    addField("tokens", &TokenSplitConfig::tokens,
             "An SQL expression specifiying the list of tokens to separate.");
    addField("splitchars", &TokenSplitConfig::splitchars,
             "A string containing the list of possible split characters. "
             "Each character in the list is interpreted as a splitchar. ",
             Utf8String("&lt;space&gt;,"));
    addField("splitcharToInsert", &TokenSplitConfig::splitcharToInsert,
             "A string containing the split character to insert if none of the characters "
             "in 'splitchars' are already present.",
             Utf8String("&lt;space&gt;"));
}

/*****************************************************************************/
/* TOKEN SPLIT FUNCTION                                                      */
/*****************************************************************************/

TokenSplit::
TokenSplit(MldbServer * owner,
            PolyConfig config,
            const std::function<bool (const Json::Value &)> & onProgress)
    : Function(owner)
{
    functionConfig = config.params.convert<TokenSplitConfig>();   
    SqlExpressionMldbContext context(owner);
 
    //get all values from the dataset and add them to our dictionary of tokens
    auto aggregator = [&] (const MatrixNamedRow & row) {
            for (auto & c: row.columns) {
                const CellValue & cellValue = std::get<1>(c);
                
                dictionary.emplace_back(std::move(cellValue.toUtf8String()));
            }
            
            return true;
        };

    BoundTableExpression boundDataset;
    if (functionConfig.tokens.stm->from)
        boundDataset = functionConfig.tokens.stm->from->bind(context);

    if (boundDataset.dataset)
        iterateDataset(functionConfig.tokens.stm->select,
                       *boundDataset.dataset, boundDataset.asName, 
                       functionConfig.tokens.stm->when,
                       *functionConfig.tokens.stm->where,
                       aggregator,
                       functionConfig.tokens.stm->orderBy,
                       functionConfig.tokens.stm->offset,
                       functionConfig.tokens.stm->limit,
                       onProgress);
    else { // query containing only a select (e.g. select "token1", "token2", "token3")
        std::vector<MatrixNamedRow> rows  = queryWithoutDataset(*functionConfig.tokens.stm, context);
        std::for_each(rows.begin(), rows.end(), aggregator);
    }
    
    // sorting is important here - it is used to optimize the tokenization
    std::sort(dictionary.begin(), dictionary.end());
}

Any
TokenSplit::
getStatus() const
{
    return Any();
}

FunctionOutput
TokenSplit::
apply(const FunctionApplier & applier,
      const FunctionContext & context) const
{
    //The whole thing is a bit contrived because UTF8 strings dont have direct access 
    FunctionOutput result;

    const ExpressionValue & text = context.get<ExpressionValue>("text");
    Utf8String textstring = text.toUtf8String();

    auto startIt = textstring.begin();
    std::vector<pair<int, int> > insertionPos;
    int startPos = 0;

    //Parse the input string and note locations before and after a token
    while (startIt != textstring.end()) {
        auto it = startIt;
        ++it;
        int pos = startPos + 1;
        int endPos = startPos;
        auto foundIter = it;
        do {                        
            Utf8String subString(startIt, it);
            bool found = false;
            bool startFound = false;
            for (auto& token : dictionary) {

                if (token == subString) {
                    //found an exact token, but there could be a longer one
                    found = true;
                }
                else if (token.startsWith(subString)) {
                    //found a token that starts with the sub string
                    startFound = true;
                }
                else if (found || startFound) {
                    //because its sorted, we can stop here
                    break;
                }
            }

            if (found) {
                endPos = pos;
                foundIter = it;
            }

            if (startFound && it != textstring.end()) {
                //move the end cursor but keep the start one there
                ++it;
                ++pos;
            }
            else {
                if (endPos != startPos) {
                    //found a token

                    // Dont insert a separator at the beginning if we already have one from a prior token
                    if (startPos > 0 && (insertionPos.empty() || insertionPos.back().first < startPos)) {
                        insertionPos.push_back(make_pair(startPos-1, startPos));
                    }

                    //we'll check afterwards if there is already a separator at this position
                    insertionPos.push_back(make_pair(endPos, endPos));

                    //move the start cursor at the end of the token
                    startIt = it;
                    startPos = pos;               
                }
                else
                {
                    ++startIt;
                    ++startPos;
                }

                break;
            }
        } while (true); //we either move the start cursor or break

        if (it == textstring.end())
            break;
    }

    //insert separators if there arent any

    //check if there is already a separator

    auto& splitchars = functionConfig.splitchars;

    std::vector<int> insertionNeeded;
    auto it = textstring.begin();
    int pos = 0;
    for (auto insert : insertionPos) {

        //move to the position
        while (pos != insert.first) {
            it++;
            pos++;
        }

        if (it == textstring.end())
            break;

        //Check if that character is one of several possible separators
        auto splitCharIt = splitchars.begin();
        bool found = false;
        while (splitCharIt != splitchars.end()) {
            if (*it == *splitCharIt) {
                found = true;
                break;
            }
            ++splitCharIt;
        }

        //if not, mark for insertion
        if (!found)
            insertionNeeded.push_back(insert.second);
    }

    Utf8String output;
    if (insertionNeeded.empty()) {
        output = textstring; //nothing to do
    }
    else {
        auto it = textstring.begin();
        auto start = it;
        int pos = 0;
        for (int i = 0; i < insertionNeeded.size(); ++i) {
            int insertPos = insertionNeeded[i];            
            while (pos != insertPos) {
                it++;
                pos++;
            }

            output += Utf8String(start, it) + functionConfig.splitcharToInsert;
            start = it;
        }

        output += Utf8String(start, textstring.end());
    }

    result.set("output", ExpressionValue(output, text.getEffectiveTimestamp()));
    
    return result;
}

FunctionInfo
TokenSplit::
getFunctionInfo() const
{
    FunctionInfo result;

    result.input.addAtomValue("text");
    result.output.addAtomValue("output");
    
    return result;
}

namespace {

RegisterFunctionType<TokenSplit, TokenSplitConfig>
regSvdEmbedRow(builtinPackage(),
	       "tokensplit",
               "Insert spaces after tokens from a dictionary",
               "functions/TokenSplit.md.html");

} // file scope

} // namespace MLDB
} // namespace Datacratic
