/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

#include "misc.h"
#include "nnue/evaluate_nnue.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

namespace Eval {

std::string currentEvalFileName = "None";

// Tries to load a NNUE network at startup time, or when the engine
// receives a UCI command "setoption name EvalFile value .*.nnue"
// The name of the NNUE network is always retrieved from the EvalFile option.
// We search the given network in two locations: in the active working directory and
// in the engine directory.
EvalFile NNUE::load_networks(const std::string& rootDirectory,
                             const OptionsMap&  options,
                             EvalFile           evalFile) {

    std::string user_eval_file = options[evalFile.optionName];

    if (user_eval_file.empty())
        user_eval_file = evalFile.defaultName;

    std::vector<std::string> dirs = {"", rootDirectory};

    for (const std::string& directory : dirs)
        if (evalFile.current != user_eval_file)
        {
            std::stringstream ss          = read_zipped_nnue(directory + user_eval_file);
            auto              description = NNUE::load_eval(ss);
            if (!description.has_value())
            {
                std::ifstream stream(directory + user_eval_file, std::ios::binary);
                description = NNUE::load_eval(stream);
            }

            if (description.has_value())
            {
                evalFile.current        = user_eval_file;
                evalFile.netDescription = description.value();
            }
        }

    return evalFile;
}

// Verifies that the last net used was loaded successfully
void NNUE::verify(const OptionsMap& options, const EvalFile& evalFile) {

    std::string user_eval_file = options[evalFile.optionName];

    if (user_eval_file.empty())
        user_eval_file = evalFile.defaultName;

    if (evalFile.current != user_eval_file)
    {

        std::string msg1 =
          "Network evaluation parameters compatible with the engine must be available.";
        std::string msg2 = "The network file " + user_eval_file + " was not loaded successfully.";
        std::string msg3 = "The UCI option EvalFile might need to specify the full path, "
                           "including the directory name, to the network file.";
        std::string msg4 =
          "The default net can be downloaded from: "
          "https://github.com/official-pikafish/Networks/releases/download/master-net/"
          + evalFile.defaultName;
        std::string msg5 = "The engine will be terminated now.";

        sync_cout << "info string ERROR: " << msg1 << sync_endl;
        sync_cout << "info string ERROR: " << msg2 << sync_endl;
        sync_cout << "info string ERROR: " << msg3 << sync_endl;
        sync_cout << "info string ERROR: " << msg4 << sync_endl;
        sync_cout << "info string ERROR: " << msg5 << sync_endl;

        exit(EXIT_FAILURE);
    }

    sync_cout << "info string NNUE evaluation using " << user_eval_file << " enabled" << sync_endl;
}
}


// Returns a static, purely materialistic evaluation of the position from
// the point of view of the given color. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos, Color c) {
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + AdvisorValue * (pos.count<ADVISOR>(c) - pos.count<ADVISOR>(~c))
         + BishopValue * (pos.count<BISHOP>(c) - pos.count<BISHOP>(~c))
         + (pos.major_material(c) - pos.major_material(~c));
}

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Position& pos, int optimism, Value alpha, Value beta) {

    assert(!pos.checkers());

    int   v;
    Color stm        = pos.side_to_move();
    int   shuffling  = pos.rule60_count();
    int   simpleEval = simple_eval(pos, stm);
    bool  psqtOnly   = alpha - 2500 > simpleEval || simpleEval > beta + 2500;

    int   nnueComplexity;
    Value nnue = NNUE::evaluate(pos, true, &nnueComplexity, psqtOnly);

    // Blend optimism and eval with nnue complexity and material imbalance
    optimism += optimism * (nnueComplexity + std::abs(simpleEval - nnue)) / 781;
    nnue -= nnue * (nnueComplexity + std::abs(simpleEval - nnue)) / 30087;

    int mm = pos.major_material() / 41;
    v      = (nnue * (568 + mm) + optimism * (138 + mm)) / 1434;

    // Damp down the evaluation linearly when shuffling
    v = v * (293 - shuffling) / 194;

    // Guarantee evaluation does not hit the mate range
    v = std::clamp(v, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);

    ss << '\n' << NNUE::trace(pos) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    Value v;
    v = NNUE::evaluate(pos);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCI::to_cp(v) << " (white side)\n";

    v = evaluate(pos, VALUE_ZERO, -VALUE_NONE, VALUE_NONE);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCI::to_cp(v) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Stockfish
