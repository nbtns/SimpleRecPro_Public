#include <JuceHeader.h>
#include "MainComponent.h"

class SimpleRecProApplication  : public juce::JUCEApplication
{
public:
    SimpleRecProApplication() {}

    const juce::String getApplicationName() override       { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr; // (deletes our window)
    }

    void systemRequestedQuit() override;

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
    }

    class MainWindow    : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                                          .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);

           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            setResizeLimits (1100, 700, 3840, 2160);
            centreWithSize (getWidth(), getHeight());
           #endif

            setVisible (true);
        }

        void closeButtonPressed() override
        {
            requestClose();
        }

        void requestClose()
        {
            if (auto* main = dynamic_cast<MainComponent*>(getContentComponent()))
                main->requestQuit();
            else
                JUCEApplication::getInstance()->quit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

void SimpleRecProApplication::systemRequestedQuit()
{
    if (mainWindow != nullptr)
        mainWindow->requestClose();
    else
        quit();
}

START_JUCE_APPLICATION (SimpleRecProApplication)
