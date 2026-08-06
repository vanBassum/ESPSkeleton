#pragma once

#include "AppServices.h"
#include "Board.h"
#include "StruxServices.h"
#include "LedManager/LedManager.h"

// The application layer's context: owns this product's managers and answers AppServices.
//
// This is the file a fork edits. Strux's own managers, their init order and their wiring
// all live one layer down in StruxContext, so pulling an improvement from the template
// does not touch anything here — which is the whole reason the layers were split.
//
// Adding an application manager means: create the class taking AppServices&, add an
// accessor to AppServices, add the member here, and call its Init() below. The framework
// does not need to be told it exists; the manager registers its own commands and
// settings into Strux from its Init().
class ApplicationContext : public AppServices
{
public:
    ApplicationContext(Board& board, StruxServices& strux)
        : board_(board), strux_(strux) {}

    ~ApplicationContext() = default;
    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;

    /// Bring the application up. Called last: every manager here registers into the
    /// framework, so the framework has to be ready before any of this runs.
    void Init()
    {
        ledManager_.Init();
    }

    StruxServices& getStrux() override { return strux_; }
    Board& getBoard() override { return board_; }
    LedManager& getLedManager() override { return ledManager_; }

private:
    Board& board_;
    StruxServices& strux_;

    LedManager ledManager_{*this};
};
