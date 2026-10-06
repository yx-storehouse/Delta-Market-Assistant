#pragma once

class MainWindow;
class QString;
namespace relink::workspace {
class WorkspaceController;
bool runWorkspaceUiSelfTest(MainWindow& window, WorkspaceController& workspace, const QString& outputDirectory);
}
