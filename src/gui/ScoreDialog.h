// File > Sheet music: MIDI tracks as a score, previewed page by page and exported as PDF (to print or
// send) or MusicXML (to go on in MuseScore, Sibelius, Finale or Dorico).

#pragma once

#include "core/Score.h"

#include <QDialog>
#include <QTimer>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class ScoreView;

class ScoreDialog : public QDialog {
    Q_OBJECT

public:
    struct Track {
        QString name;
        int index = -1; // in the session
        QString file;   // its MIDI file
    };

    ScoreDialog(const QString& title, const QString& key, double tempo, const QString& folder, QWidget* parent = nullptr);

    void setTracks(const std::vector<Track>& tracks, int selected); // MIDI tracks, drums left out
    void setSong(const QString& key, double tempo);
    void setChosen(const std::vector<int>& tracks); // session indices to put in the score (tests)
    bool exportTo(const QString& path, QString& error); // .pdf, else MusicXML
    int pages() const;

    // the score of these MIDI files, and writing one: PDF for a .pdf path, else MusicXML
    static chost::Score build(const std::vector<Track>& tracks, const chost::ScoreOptions& options, chost::StaffMode staff,
                              std::string& error);
    static bool write(const chost::Score& score, const QString& path, QString& error, int* pages = nullptr);

private:
    void rebuild();
    void exportAs(bool pdf);
    std::vector<Track> chosen() const;

    QString fSongKey;
    double fTempo;
    QString fFolder;
    std::vector<Track> fTracks;
    chost::Score fScore;
    QTimer fRebuild;

    QListWidget* fList = nullptr;
    QLineEdit* fTitle = nullptr;
    QComboBox* fKey = nullptr;
    QComboBox* fStaff = nullptr;
    QComboBox* fGrid = nullptr;
    QComboBox* fZoom = nullptr;
    QPushButton* fPdf = nullptr;
    QPushButton* fXml = nullptr;
    QLabel* fStatus = nullptr;
    ScoreView* fView = nullptr;
};
