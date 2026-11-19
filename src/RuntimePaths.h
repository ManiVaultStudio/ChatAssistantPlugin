#pragma once
#include <QString>

/** Discovers data bundled with the host, or uses user-selected local paths. */
class RuntimePaths final {
  public:
    static QString discoverPackage();
    static QString discoverExecutable();
    static QString modelFile(const QString &packageDirectory);
};
