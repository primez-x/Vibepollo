using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace VibepolloInstaller {
  internal static class BuildFlavor {
#if UNINSTALL_ONLY
    public static readonly bool IsUninstallOnly = true;
#else
    public static readonly bool IsUninstallOnly = false;
#endif
  }

  internal static class Program {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SetCurrentProcessExplicitAppUserModelID(string appID);

    private static void TrySetExplicitAppUserModelId() {
      try {
        SetCurrentProcessExplicitAppUserModelID(ShellIdentity.InstallerAppUserModelId);
      } catch {
      }
    }

    [STAThread]
    private static int Main(string[] args) {
      if (InstallerArguments.IsHelpRequested(args)) {
        InstallerArguments.WriteHelp();
        return 0;
      }

      var parsed = InstallerArguments.Parse(args);
      if (BuildFlavor.IsUninstallOnly) {
        parsed.UninstallUiRequested = true;
      }
      if (parsed.UninstallUiRequested && !parsed.ShowUi && !parsed.InternalElevatedUninstall) {
        var uninstallResult = InstallerRunner.RunInteractiveUninstall(parsed, false, false);
        if (!string.IsNullOrWhiteSpace(uninstallResult.Message)) {
          Console.WriteLine(uninstallResult.Message);
        }
        return uninstallResult.ExitCode == 1605 ? 0 : uninstallResult.ExitCode;
      }
      if (parsed.InternalElevatedInstall) {
        var installPath = string.IsNullOrWhiteSpace(parsed.InternalInstallPath)
          ? InstallerRunner.DefaultInstallDirectory
          : parsed.InternalInstallPath;
        var internalInstall = InstallerRunner.RunInteractiveInstall(
          parsed,
          installPath,
          parsed.InternalInstallVirtualDisplay,
          parsed.InternalInstallSaveLogs,
          false);
        InstallerRunner.TryWriteInternalInstallResult(parsed.InternalInstallResultPath, internalInstall);
        return internalInstall.ExitCode;
      }

      if (parsed.InternalElevatedUninstall) {
        var internalUninstall = InstallerRunner.RunInteractiveUninstall(
          parsed,
          parsed.InternalUninstallFactoryReset,
          parsed.InternalUninstallRemoveVirtualDisplayDriver,
          false);
        return internalUninstall.ExitCode;
      }

      if (!parsed.ShowUi) {
        var cliResult = InstallerRunner.RunCli(parsed);
        InstallerRunner.TryWriteInternalInstallResult(parsed.InternalInstallResultPath, cliResult);
        if (!string.IsNullOrWhiteSpace(cliResult.Message)) {
          Console.WriteLine(cliResult.Message);
        }
        return cliResult.ExitCode;
      }

      TrySetExplicitAppUserModelId();

      var app = new Application {
        ShutdownMode = ShutdownMode.OnMainWindowClose
      };
      var window = new InstallerWindow(parsed);
      app.Run(window);
      return window.ProcessExitCode;
    }
  }

  internal sealed class InstallerWindow : Window {
    private readonly InstallerArguments _arguments;
    private readonly Border _installSection;
    private readonly Border _installVirtualDisplaySection;
    private readonly TextBlock _installLocationTitleText;
    private readonly TextBlock _installLocationHintText;
    private readonly Grid _installPathGrid;
    private readonly TextBox _installPathTextBox;
    private readonly ComboBox _virtualDisplayDriverComboBox;
    private readonly TextBlock _statusText;
    private readonly TextBlock _statusDetailText;
    private readonly ProgressBar _progressBar;
    private readonly Button _browseButton;
    private readonly Button _continueButton;
    private readonly Button _uninstallButton;
    private readonly Button _licenseButton;
    private readonly Button _closeButton;
    private readonly Button _titleCloseButton;
    private readonly Grid _overlayGrid;
    private readonly Border _overlayAccentBar;
    private readonly TextBlock _overlayTitleText;
    private readonly TextBlock _overlayMessageText;
    private readonly TextBlock _overlayHintText;
    private readonly ProgressBar _overlayAutoCloseProgressBar;
    private readonly StackPanel _overlayContentHost;
    private readonly Button _overlayPrimaryButton;
    private readonly Button _overlaySecondaryButton;
    private TaskCompletionSource<string> _overlayTcs;
    private DispatcherTimer _overlayAutoCloseTimer;
    private int _overlayAutoCloseSecondsRemaining;
    private int _overlayAutoCloseTotalSeconds;
    private DateTime _overlayAutoCloseDeadlineUtc;
    private bool _isBusy;
    private readonly bool _uninstallUiRequested;
    private readonly Brush _statusNormalBrush = new SolidColorBrush(Color.FromRgb(245, 249, 255));
    private readonly Brush _statusBusyBrush = new SolidColorBrush(Color.FromRgb(147, 197, 253));
    private readonly Brush _statusSuccessBrush = new SolidColorBrush(Color.FromRgb(16, 185, 129));
    private readonly Brush _statusWarningBrush = new SolidColorBrush(Color.FromRgb(251, 191, 36));
    private readonly Brush _statusErrorBrush = new SolidColorBrush(Color.FromRgb(255, 178, 196));
    private readonly Version _bundleVersion;
    private readonly InstallerRunner.InstalledProductInfo _installedProduct;
    private readonly InstallerRunner.InstalledProductInfo _legacySunshineProduct;
    private readonly InstallerRunner.LegacySunshineRegistration _legacySunshineRegistration;
    private readonly InstallerRunner.LegacySunshineRegistration _legacyApolloRegistration;
    private InstallerRunner.PayloadMsiInfo _payloadMsiInfo;
    private readonly string _licenseText;
    private readonly string _preferredInstallDirectory;
    private readonly bool _useSudoVdaSelectedInConfig;
    private readonly bool _showInstallVirtualDisplayOption;
    private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    private static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const int SW_RESTORE = 9;

    public int ProcessExitCode { get; private set; }

    public InstallerWindow(InstallerArguments arguments) {
      _arguments = arguments;
      _bundleVersion = Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0, 0, 0);
      _licenseText = LoadEmbeddedLicenseText();
      _installedProduct = InstallerRunner.GetInstalledVibepolloProduct();
      _legacySunshineProduct = InstallerRunner.GetInstalledSunshineProduct();
      _legacySunshineRegistration = InstallerRunner.GetLegacySunshineRegistration();
      _legacyApolloRegistration = InstallerRunner.GetLegacyApolloRegistration();
      // Read the payload metadata before the UI chooses its version text and
      // install button label.  MsiOpenPackageEx uses ignore-machine-state below
      // so this inspects the package database without triggering maintenance on
      // already-installed products.
      _payloadMsiInfo = InstallerRunner.TryGetPayloadMsiInfo(_arguments);
      _preferredInstallDirectory = ResolvePreferredInstallDirectory();
      _useSudoVdaSelectedInConfig = IsSudoVdaSelectedInConfiguration(_preferredInstallDirectory);
      _uninstallUiRequested = BuildFlavor.IsUninstallOnly || arguments.UninstallUiRequested;
      var showInstallLocation = !BuildFlavor.IsUninstallOnly && _installedProduct == null;
      _showInstallVirtualDisplayOption = !BuildFlavor.IsUninstallOnly;
      var showInstallOptions = showInstallLocation || _showInstallVirtualDisplayOption;
      var useCompactUpdateLayout = !BuildFlavor.IsUninstallOnly && _installedProduct != null && !showInstallOptions;
      var displayVersion = GetTargetVersionText();
      Title = (BuildFlavor.IsUninstallOnly ? "Vibepollo Uninstaller v" : "Vibepollo Installer v") + displayVersion;
      Width = 720;
      Height = showInstallOptions ? 620 : useCompactUpdateLayout ? 430 : 500;
      MinWidth = 690;
      MinHeight = showInstallOptions ? 580 : useCompactUpdateLayout ? 410 : 470;
      WindowStartupLocation = WindowStartupLocation.CenterScreen;
      ResizeMode = ResizeMode.CanMinimize;
      WindowStyle = WindowStyle.None;
      AllowsTransparency = false;
      Background = CreateBackgroundBrush();
      FontFamily = new FontFamily("Segoe UI");

      var root = new Grid {
        Background = new SolidColorBrush(Color.FromRgb(6, 10, 24))
      };
      root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
      Content = root;

      var titleBar = new Border {
        Height = 36,
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 30)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(52, 64, 94)),
        BorderThickness = new Thickness(0, 0, 0, 1)
      };
      titleBar.MouseLeftButtonDown += TitleBarMouseLeftButtonDown;
      Grid.SetRow(titleBar, 0);
      root.Children.Add(titleBar);

      var titleGrid = new Grid();
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition());
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      titleBar.Child = titleGrid;

      var titleBarText = new TextBlock {
        Text = Title,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        FontSize = 12,
        Margin = new Thickness(14, 0, 0, 0),
        VerticalAlignment = VerticalAlignment.Center
      };
      titleGrid.Children.Add(titleBarText);

      var titleMinimizeButton = new Button {
        Content = "−",
        Width = 42,
        Height = 28,
        Margin = new Thickness(0, 4, 0, 4),
        Background = Brushes.Transparent,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        BorderBrush = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        FontSize = 16,
        ToolTip = "Minimize"
      };
      titleMinimizeButton.MouseEnter += TitleMinimizeMouseEnter;
      titleMinimizeButton.MouseLeave += TitleButtonMouseLeave;
      titleMinimizeButton.Click += MinimizeClicked;
      ApplyFlatButtonTemplate(titleMinimizeButton, 4);
      Grid.SetColumn(titleMinimizeButton, 1);
      titleGrid.Children.Add(titleMinimizeButton);

      _titleCloseButton = new Button {
        Content = "✕",
        Width = 42,
        Height = 28,
        Margin = new Thickness(2, 4, 8, 4),
        Background = Brushes.Transparent,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        BorderBrush = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        FontSize = 12,
        ToolTip = "Close"
      };
      _titleCloseButton.MouseEnter += TitleCloseMouseEnter;
      _titleCloseButton.MouseLeave += TitleButtonMouseLeave;
      _titleCloseButton.Click += (sender, eventArgs) => Close();
      ApplyFlatButtonTemplate(_titleCloseButton, 4);
      Grid.SetColumn(_titleCloseButton, 2);
      titleGrid.Children.Add(_titleCloseButton);

      var card = new Border {
        CornerRadius = new CornerRadius(18),
        Margin = new Thickness(20, 10, 20, 12),
        Padding = new Thickness(20),
        Background = new SolidColorBrush(Color.FromArgb(238, 14, 20, 36)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(145, 99, 102, 241)),
        BorderThickness = new Thickness(1.2),
        VerticalAlignment = VerticalAlignment.Top
      };
      Grid.SetRow(card, 1);
      root.Children.Add(card);

      _overlayGrid = new Grid {
        Background = new SolidColorBrush(Color.FromArgb(172, 4, 8, 18)),
        Visibility = Visibility.Collapsed
      };
      Panel.SetZIndex(_overlayGrid, 50);
      Grid.SetRowSpan(_overlayGrid, 2);
      root.Children.Add(_overlayGrid);

      var overlayCard = new Border {
        CornerRadius = new CornerRadius(16),
        Padding = new Thickness(20),
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 32)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(86, 102, 146)),
        BorderThickness = new Thickness(1.2),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        Width = 540,
        MaxHeight = 390
      };
      _overlayGrid.Children.Add(overlayCard);

      var overlayLayout = new Grid();
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      overlayCard.Child = overlayLayout;

      _overlayAccentBar = new Border {
        Height = 4,
        CornerRadius = new CornerRadius(3),
        Margin = new Thickness(0, 0, 0, 14),
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241))
      };
      Grid.SetRow(_overlayAccentBar, 0);
      overlayLayout.Children.Add(_overlayAccentBar);

      var overlayBodyScroll = new ScrollViewer {
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        Margin = new Thickness(0, 0, 0, 12)
      };
      Grid.SetRow(overlayBodyScroll, 1);
      overlayLayout.Children.Add(overlayBodyScroll);

      var overlayStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      overlayBodyScroll.Content = overlayStack;

      _overlayTitleText = new TextBlock {
        FontSize = 17,
        FontWeight = FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayTitleText);

      _overlayMessageText = new TextBlock {
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(210, 222, 242)),
        Margin = new Thickness(0, 0, 0, 10),
        LineHeight = 19,
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayMessageText);

      _overlayHintText = new TextBlock {
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        Margin = new Thickness(0, 0, 0, 8),
        Visibility = Visibility.Collapsed,
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayHintText);

      _overlayAutoCloseProgressBar = new ProgressBar {
        Height = 4,
        Minimum = 0,
        Maximum = 5,
        Value = 0,
        Visibility = Visibility.Collapsed,
        Margin = new Thickness(2, 0, 2, 12),
        Foreground = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Background = new SolidColorBrush(Color.FromRgb(24, 34, 58)),
        BorderThickness = new Thickness(0)
      };
      overlayStack.Children.Add(_overlayAutoCloseProgressBar);

      _overlayContentHost = new StackPanel {
        Orientation = Orientation.Vertical,
        Margin = new Thickness(0, 0, 0, 12)
      };
      overlayStack.Children.Add(_overlayContentHost);

      var overlayButtons = new Grid {
        Margin = new Thickness(0, 6, 0, 0)
      };
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition());
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      Grid.SetRow(overlayButtons, 2);
      overlayLayout.Children.Add(overlayButtons);

      _overlaySecondaryButton = new Button {
        Content = "Cancel",
        Height = 38,
        MinWidth = 96,
        Margin = new Thickness(0, 0, 10, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141))
      };
      _overlaySecondaryButton.Click += (sender, eventArgs) => ResolveOverlay("secondary");
      ApplyFlatButtonTemplate(_overlaySecondaryButton, 8);
      Grid.SetColumn(_overlaySecondaryButton, 1);
      overlayButtons.Children.Add(_overlaySecondaryButton);

      _overlayPrimaryButton = new Button {
        Content = "OK",
        Height = 38,
        MinWidth = 108,
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252))
      };
      _overlayPrimaryButton.Click += (sender, eventArgs) => ResolveOverlay("primary");
      ApplyFlatButtonTemplate(_overlayPrimaryButton, 8);
      Grid.SetColumn(_overlayPrimaryButton, 2);
      overlayButtons.Children.Add(_overlayPrimaryButton);

      var cardGrid = new Grid();
      cardGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      cardGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      card.Child = cardGrid;

      var contentStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      Grid.SetRow(contentStack, 0);
      cardGrid.Children.Add(contentStack);

      _installSection = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(16),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(44, 99, 102, 241)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(112, 128, 133, 255)),
        BorderThickness = new Thickness(1)
      };
      contentStack.Children.Add(_installSection);

      var installStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      _installSection.Child = installStack;

      _installLocationTitleText = new TextBlock {
        Text = "Install Location",
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = Brushes.White,
        Margin = new Thickness(0, 0, 0, 4)
      };
      installStack.Children.Add(_installLocationTitleText);

      _installLocationHintText = new TextBlock {
        Text = "Choose where Vibepollo will be installed. The default is recommended.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(209, 222, 241)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      };
      installStack.Children.Add(_installLocationHintText);

      _installPathGrid = new Grid();
      _installPathGrid.ColumnDefinitions.Add(new ColumnDefinition());
      _installPathGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      installStack.Children.Add(_installPathGrid);

      _installPathTextBox = new TextBox {
        FontSize = 13,
        Height = 36,
        Padding = new Thickness(10, 6, 10, 6),
        VerticalContentAlignment = VerticalAlignment.Center,
        Text = _preferredInstallDirectory,
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 30)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(96, 111, 171)),
        CaretBrush = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        ToolTip = "Used when installing or updating Vibepollo"
      };
      _installPathGrid.Children.Add(_installPathTextBox);

      _browseButton = new Button {
        Content = "_Browse...",
        Margin = new Thickness(10, 0, 0, 0),
        MinWidth = 104,
        Height = 36,
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        ToolTip = "Choose a different install location"
      };
      _browseButton.Click += BrowseClicked;
      ApplyFlatButtonTemplate(_browseButton, 7);
      Grid.SetColumn(_browseButton, 1);
      _installPathGrid.Children.Add(_browseButton);

      var virtualDisplayDriverLabel = new TextBlock {
        Text = "Virtual display driver",
        FontSize = 13,
        FontWeight = FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 0, 0, 6)
      };

      _virtualDisplayDriverComboBox = new ComboBox {
        FontSize = 13,
        MinHeight = 32,
        MaxWidth = 340,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalContentAlignment = VerticalAlignment.Center,
        Margin = new Thickness(0, 0, 0, 8),
        ToolTip = "Choose which bundled virtual display driver Vibepollo uses. The Vibepollo Display Driver is the recommended default."
      };
      _virtualDisplayDriverComboBox.Items.Add(new ComboBoxItem {
        Content = "Vibepollo Display Driver (recommended)"
      });
      _virtualDisplayDriverComboBox.Items.Add(new ComboBoxItem {
        Content = "SudoVDA (legacy)"
      });
      _virtualDisplayDriverComboBox.SelectedIndex = _useSudoVdaSelectedInConfig ? 1 : 0;

      var installVirtualDisplayHintText = new TextBlock {
        Text = "The Vibepollo Display Driver is installed and selected by default for virtual displays. Pick SudoVDA (legacy) only if you need to keep using the previous driver.",
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(190, 208, 236)),
        TextWrapping = TextWrapping.Wrap
      };

      var tipsSection = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(16),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(34, 56, 189, 248)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(92, 99, 157, 219)),
        BorderThickness = new Thickness(1)
      };
      contentStack.Children.Add(tipsSection);

      var tipsStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      tipsSection.Child = tipsStack;

      tipsStack.Children.Add(new TextBlock {
        Text = "Quick Tips",
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = Brushes.White,
        Margin = new Thickness(0, 0, 0, 4)
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "You can install or upgrade Vibepollo while actively streaming. No system restart is required. "
          + "After you click Install or Upgrade, the current streaming session will end, then you can usually "
          + "start streaming again after about 1–2 minutes without issues.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(211, 220, 246)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "You can also install from an SSH session on this host (run in an elevated shell):",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        Margin = new Thickness(0, 0, 0, 6),
        TextWrapping = TextWrapping.Wrap
      });

      tipsStack.Children.Add(new TextBox {
        Text = "VibepolloSetup.exe /qn /norestart",
        IsReadOnly = true,
        FontFamily = new FontFamily("Consolas"),
        FontSize = 12.5,
        Margin = new Thickness(0, 0, 0, 8),
        Padding = new Thickness(10, 8, 10, 8),
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        CaretBrush = new SolidColorBrush(Color.FromRgb(226, 235, 250))
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "Click the buttons below to proceed.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(211, 220, 246)),
        Margin = new Thickness(0, 0, 0, 0),
        TextWrapping = TextWrapping.Wrap
      });

      _installVirtualDisplaySection = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(16),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(44, 99, 102, 241)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(112, 128, 133, 255)),
        BorderThickness = new Thickness(1)
      };
      contentStack.Children.Add(_installVirtualDisplaySection);

      var driverStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      _installVirtualDisplaySection.Child = driverStack;
      driverStack.Children.Add(virtualDisplayDriverLabel);
      driverStack.Children.Add(_virtualDisplayDriverComboBox);
      driverStack.Children.Add(installVirtualDisplayHintText);

      var divider = new System.Windows.Shapes.Rectangle {
        Height = 1,
        Fill = new SolidColorBrush(Color.FromArgb(120, 88, 104, 124)),
        Margin = new Thickness(0, 0, 0, 10)
      };
      contentStack.Children.Add(divider);

      var statusCard = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(14, 10, 14, 10),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(38, 56, 189, 248)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(92, 99, 157, 219)),
        BorderThickness = new Thickness(1)
      };
      statusCard.Visibility = Visibility.Collapsed;
      contentStack.Children.Add(statusCard);

      var statusStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      statusCard.Child = statusStack;

      statusStack.Children.Add(new TextBlock {
        Text = "STATUS",
        FontSize = 11,
        FontWeight = FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(176, 207, 238)),
        Margin = new Thickness(0, 0, 0, 2)
      });

      _statusText = new TextBlock {
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = _statusNormalBrush,
        Margin = new Thickness(0, 0, 0, 2),
        TextWrapping = TextWrapping.Wrap
      };
      statusStack.Children.Add(_statusText);

      _statusDetailText = new TextBlock {
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        TextWrapping = TextWrapping.Wrap
      };
      statusStack.Children.Add(_statusDetailText);

      var footerGrid = new Grid {
        Margin = new Thickness(0)
      };
      footerGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      footerGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      Grid.SetRow(footerGrid, 1);
      cardGrid.Children.Add(footerGrid);

      _progressBar = new ProgressBar {
        Height = 4,
        IsIndeterminate = true,
        Visibility = Visibility.Collapsed,
        Foreground = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Margin = new Thickness(8, 4, 8, 8)
      };
      Grid.SetRow(_progressBar, 0);
      footerGrid.Children.Add(_progressBar);

      var buttonRow = new Grid();
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition());
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      Grid.SetRow(buttonRow, 1);
      footerGrid.Children.Add(buttonRow);

      var buttonHint = new TextBlock {
        Text = "",
        Foreground = new SolidColorBrush(Color.FromRgb(195, 209, 232)),
        FontSize = 12,
        TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(0, 0, 8, 0),
        VerticalAlignment = VerticalAlignment.Center
      };
      buttonRow.Children.Add(buttonHint);

      _continueButton = new Button {
        Content = "Next",
        Height = 40,
        MinWidth = 136,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        BorderThickness = new Thickness(1.5),
        IsDefault = true,
        ToolTip = "Run the selected action"
      };
      _continueButton.MouseEnter += ContinueButtonMouseEnter;
      _continueButton.MouseLeave += ContinueButtonMouseLeave;
      _continueButton.Click += ContinueClicked;
      ApplyFlatButtonTemplate(_continueButton, 8);
      Grid.SetColumn(_continueButton, 1);
      buttonRow.Children.Add(_continueButton);

      _uninstallButton = new Button {
        Content = "Uninstall Vibepollo",
        Height = 40,
        MinWidth = 152,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(225, 29, 72)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(251, 113, 133)),
        BorderThickness = new Thickness(1.5),
        Visibility = Visibility.Visible
      };
      _uninstallButton.MouseEnter += UninstallButtonMouseEnter;
      _uninstallButton.MouseLeave += UninstallButtonMouseLeave;
      _uninstallButton.Click += UninstallNowClicked;
      ApplyFlatButtonTemplate(_uninstallButton, 8);
      Grid.SetColumn(_uninstallButton, 2);
      buttonRow.Children.Add(_uninstallButton);

      _licenseButton = new Button {
        Content = "_License",
        Height = 40,
        MinWidth = 102,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        ToolTip = "View software license"
      };
      _licenseButton.Click += LicenseClicked;
      ApplyFlatButtonTemplate(_licenseButton, 8);
      Grid.SetColumn(_licenseButton, 3);
      buttonRow.Children.Add(_licenseButton);

      _closeButton = new Button {
        Content = "Cl_ose",
        Height = 40,
        MinWidth = 102,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        IsCancel = true,
        ToolTip = "Close this installer window"
      };
      _closeButton.Click += (sender, eventArgs) => Close();
      ApplyFlatButtonTemplate(_closeButton, 8);
      Grid.SetColumn(_closeButton, 4);
      buttonRow.Children.Add(_closeButton);

      _continueButton.Content = BuildFlavor.IsUninstallOnly ? "Uninstall Vibepollo" : BuildInstallButtonLabel();
      if (_uninstallUiRequested && _installedProduct == null) {
        SetStatus(
          "Vibepollo is not installed.",
          BuildFlavor.IsUninstallOnly
            ? "No uninstall action is required."
            : "Uninstall is unavailable. Choose Install Vibepollo to continue.",
          _statusNormalBrush);
      } else {
        SetStatus("Ready.", string.Empty, _statusNormalBrush);
      }
      UpdateActionUiState();
      Loaded += InstallerWindowLoaded;
    }

    private static Brush CreateBackgroundBrush() {
      var brush = new LinearGradientBrush();
      brush.StartPoint = new Point(0, 0);
      brush.EndPoint = new Point(1, 1);
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(6, 10, 24), 0.0));
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(14, 20, 36), 0.42));
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(12, 18, 34), 1.0));
      return brush;
    }

    private static void ApplyFlatButtonTemplate(Button button, double cornerRadius) {
      button.OverridesDefaultStyle = true;
      button.Template = CreateFlatButtonTemplate(cornerRadius);
      button.FocusVisualStyle = null;
      button.Cursor = Cursors.Hand;
    }

    private static ControlTemplate CreateFlatButtonTemplate(double cornerRadius) {
      var border = new FrameworkElementFactory(typeof(Border));
      border.SetValue(Border.CornerRadiusProperty, new CornerRadius(cornerRadius));
      border.SetValue(Border.SnapsToDevicePixelsProperty, true);
      border.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BackgroundProperty));
      border.SetValue(Border.BorderBrushProperty, new TemplateBindingExtension(Control.BorderBrushProperty));
      border.SetValue(Border.BorderThicknessProperty, new TemplateBindingExtension(Control.BorderThicknessProperty));

      var content = new FrameworkElementFactory(typeof(ContentPresenter));
      content.SetValue(ContentPresenter.RecognizesAccessKeyProperty, true);
      content.SetValue(ContentPresenter.SnapsToDevicePixelsProperty, true);
      content.SetValue(ContentPresenter.ContentProperty, new TemplateBindingExtension(ContentControl.ContentProperty));
      content.SetValue(ContentPresenter.ContentTemplateProperty, new TemplateBindingExtension(ContentControl.ContentTemplateProperty));
      content.SetValue(ContentPresenter.HorizontalAlignmentProperty, new TemplateBindingExtension(Control.HorizontalContentAlignmentProperty));
      content.SetValue(ContentPresenter.VerticalAlignmentProperty, new TemplateBindingExtension(Control.VerticalContentAlignmentProperty));
      content.SetValue(ContentPresenter.MarginProperty, new TemplateBindingExtension(Control.PaddingProperty));
      content.SetValue(TextElement.ForegroundProperty, new TemplateBindingExtension(Control.ForegroundProperty));
      border.AppendChild(content);

      var template = new ControlTemplate(typeof(Button));
      template.VisualTree = border;

      var pressedTrigger = new Trigger {
        Property = Button.IsPressedProperty,
        Value = true
      };
      pressedTrigger.Setters.Add(new Setter(UIElement.OpacityProperty, 0.92));
      template.Triggers.Add(pressedTrigger);

      var disabledTrigger = new Trigger {
        Property = UIElement.IsEnabledProperty,
        Value = false
      };
      disabledTrigger.Setters.Add(new Setter(UIElement.OpacityProperty, 0.58));
      template.Triggers.Add(disabledTrigger);

      return template;
    }

    private void TitleBarMouseLeftButtonDown(object sender, MouseButtonEventArgs e) {
      if (e.ChangedButton == MouseButton.Left) {
        DragMove();
      }
    }

    private void MinimizeClicked(object sender, RoutedEventArgs e) {
      WindowState = WindowState.Minimized;
    }

    private void InstallerWindowLoaded(object sender, RoutedEventArgs e) {
      BringWindowToFront();
      FocusDefaultActionControl();
      Dispatcher.BeginInvoke(new Action(() => {
        BringWindowToFront();
        FocusDefaultActionControl();
      }), DispatcherPriority.ContextIdle);

    }

    protected override void OnSourceInitialized(EventArgs e) {
      base.OnSourceInitialized(e);
      // Apply shell identity to the concrete top-level window handle as early as
      // possible so Windows does not briefly bucket the installer under the
      // installed app or temporary generic taskbar identities while WPF loads.
      ShellIdentity.TryApplyInstallerWindowIdentity(
        new WindowInteropHelper(this).Handle,
        BuildFlavor.IsUninstallOnly ? "Vibepollo Uninstaller" : "Vibepollo Installer"
      );
    }

    private void FocusDefaultActionControl() {
      if (_installSection.Visibility == Visibility.Visible) {
        _installPathTextBox.Focus();
        _installPathTextBox.SelectAll();
      } else if (BuildFlavor.IsUninstallOnly && _uninstallButton.Visibility == Visibility.Visible) {
        _uninstallButton.Focus();
      } else {
        _continueButton.Focus();
      }
    }

    private void BringWindowToFront() {
      if (WindowState == WindowState.Minimized) {
        WindowState = WindowState.Normal;
      }

      Show();
      Activate();

      var handle = new WindowInteropHelper(this).Handle;
      if (handle == IntPtr.Zero) {
        return;
      }

      ShowWindow(handle, SW_RESTORE);
      SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      BringWindowToTop(handle);
      SetForegroundWindow(handle);
    }

    protected override void OnClosed(EventArgs e) {
      StopOverlayAutoClose();
      base.OnClosed(e);
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
      IntPtr hWnd,
      IntPtr hWndInsertAfter,
      int X,
      int Y,
      int cx,
      int cy,
      uint uFlags);

    private void TitleMinimizeMouseEnter(object sender, MouseEventArgs e) {
      var button = sender as Button;
      if (button == null) {
        return;
      }
      button.Background = new SolidColorBrush(Color.FromRgb(58, 76, 122));
    }

    private void TitleCloseMouseEnter(object sender, MouseEventArgs e) {
      _titleCloseButton.Background = new SolidColorBrush(Color.FromRgb(224, 46, 90));
      _titleCloseButton.Foreground = Brushes.White;
    }

    private void TitleButtonMouseLeave(object sender, MouseEventArgs e) {
      var button = sender as Button;
      if (button == null) {
        return;
      }
      button.Background = Brushes.Transparent;
      button.Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255));
    }

    private void ContinueButtonMouseEnter(object sender, MouseEventArgs e) {
      _continueButton.Background = new SolidColorBrush(Color.FromRgb(129, 140, 248));
      _continueButton.BorderBrush = new SolidColorBrush(Color.FromRgb(199, 210, 254));
    }

    private void ContinueButtonMouseLeave(object sender, MouseEventArgs e) {
      _continueButton.Background = new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _continueButton.BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252));
    }

    private void UninstallButtonMouseEnter(object sender, MouseEventArgs e) {
      _uninstallButton.Background = new SolidColorBrush(Color.FromRgb(244, 63, 94));
      _uninstallButton.BorderBrush = new SolidColorBrush(Color.FromRgb(253, 164, 175));
    }

    private void UninstallButtonMouseLeave(object sender, MouseEventArgs e) {
      _uninstallButton.Background = new SolidColorBrush(Color.FromRgb(225, 29, 72));
      _uninstallButton.BorderBrush = new SolidColorBrush(Color.FromRgb(251, 113, 133));
    }

    private void BrowseClicked(object sender, RoutedEventArgs e) {
      var currentPath = _installPathTextBox.Text;
      if (string.IsNullOrWhiteSpace(currentPath)) {
        currentPath = _preferredInstallDirectory;
      }

      var selectedPath = ModernFolderPicker.TryPickFolder(this, "Select the Vibepollo install folder", currentPath);
      if (!string.IsNullOrWhiteSpace(selectedPath)) {
        _installPathTextBox.Text = selectedPath;
      }
    }

    private async void ContinueClicked(object sender, RoutedEventArgs e) {
      if (BuildFlavor.IsUninstallOnly) {
        await RunUninstallFlow();
        return;
      }
      await RunInstallFlow();
    }

    private async void UninstallNowClicked(object sender, RoutedEventArgs e) {
      if (_installedProduct == null) {
        SetStatus("Uninstall not started.", "No Vibepollo installation was found on this PC.", _statusNormalBrush);
        await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
        return;
      }

      await RunUninstallFlow();
    }

    private async void LicenseClicked(object sender, RoutedEventArgs e) {
      await ShowLicenseDialogAsync();
    }

    private async Task RunInstallFlow() {
      string selectedPath = null;
      string installPathFailureDetail = null;
      try {
        selectedPath = NormalizeInstallPath(_installPathTextBox.Text);
      } catch (Exception ex) {
        installPathFailureDetail = BuildFailureDetail(ex.Message);
      }
      if (!string.IsNullOrWhiteSpace(installPathFailureDetail)) {
        SetStatus("Install not started.", installPathFailureDetail, _statusErrorBrush);
        await ShowOverlayInfoAsync("Installer error", installPathFailureDetail);
        return;
      }
      _installPathTextBox.Text = selectedPath;

      var vibeshineProduct = InstallerRunner.GetInstalledVibeshineProduct();
      if (vibeshineProduct != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Sunshine ecosystem detected",
          BuildVibeshineInstallWarning(vibeshineProduct),
          "Continue with Vibepollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      var apolloProduct = InstallerRunner.GetInstalledApolloProduct();
      if (apolloProduct != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Apollo detected",
          BuildApolloInstallWarning(apolloProduct),
          "Uninstall Apollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      if (_legacyApolloRegistration != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Legacy Apollo detected",
          BuildLegacyApolloMigrationWarning(),
          "Uninstall Apollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      if (_legacySunshineProduct != null || _legacySunshineRegistration != null) {
        var proceedWithRemoval = await ShowOverlayConfirmAsync(
          "Sunshine detected",
          BuildLegacySunshineMigrationWarning(),
          "Uninstall Sunshine",
          "Cancel",
          false);
        if (!proceedWithRemoval) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      // Warn about low disk space but allow the user to proceed
      var spaceWarning = CheckDiskSpace(selectedPath);
      if (spaceWarning != null) {
        var proceed = await ShowOverlayConfirmAsync("Low disk space", spaceWarning + "\n\nContinue anyway?", "Continue", "Cancel", false);
        if (!proceed) {
          SetStatus("Install not started.", "Choose a drive with more free space.", _statusNormalBrush);
          return;
        }
      }

      await RunOperationAsync(async () => {
        var installVirtualDisplayDriver = ShouldInstallVirtualDisplayDriver();
        return await Task.Run(() => InstallerRunner.RunInteractiveInstall(
          _arguments,
          selectedPath,
          installVirtualDisplayDriver,
          false));
      }, "Install", "Installing or updating Vibepollo...", "Vibepollo installation completed.");
    }

    private bool ShouldInstallVirtualDisplayDriver() {
      return _virtualDisplayDriverComboBox.SelectedIndex != 1;
    }

    private async Task RunUninstallFlow() {
      if (_installedProduct == null) {
        SetStatus("Uninstall not started.", "No Vibepollo installation was found on this PC.", _statusNormalBrush);
        await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
        return;
      }

      var uninstallOptions = await ShowOverlayUninstallOptionsAsync();
      if (uninstallOptions == null) {
        SetStatus("Uninstall canceled.", "No changes were made.", _statusNormalBrush);
        return;
      }

      await RunOperationAsync(
        () => Task.Run(() => InstallerRunner.RunInteractiveUninstall(
          _arguments,
          uninstallOptions.Value.FactoryResetAppData,
          uninstallOptions.Value.RemoveVirtualDisplayDriver)),
        "Uninstall",
        "Removing Vibepollo...",
        "Vibepollo uninstall completed.");
    }

    private async Task RunOperationAsync(Func<Task<InstallerResult>> actionFactory, string actionLabel, string inProgressText, string successText) {
      SetBusyState(true);
      SetStatus(inProgressText, "This can take a minute. Admin approval is requested only after the operation starts.", _statusBusyBrush);
      string exceptionFailureDetail = null;
      try {
        var result = await actionFactory();
        if (result.InstallDeferredForRestart) {
          ProcessExitCode = result.ExitCode;
          var detail = string.IsNullOrWhiteSpace(result.Message)
            ? "Migration cleanup completed, but Windows must restart before the Vibepollo payload can be installed."
            : result.Message;
          if (!string.IsNullOrWhiteSpace(result.UserDetail)) {
            detail += "\n" + result.UserDetail;
          }
          SetStatus("Restart required before installation can continue.", detail, _statusWarningBrush);
          await ShowOverlayInfoAsync("Restart required", _statusText.Text);
          Close();
          return;
        }
        if (result.Succeeded) {
          ProcessExitCode = 0;
          if (result.Operation == InstallerOperation.Install && result.PartiallySucceeded) {
            var warningDetail = BuildComponentFailureDetail(result.ComponentFailures);
            if (!string.IsNullOrWhiteSpace(result.UserDetail)) {
              warningDetail += "\n" + result.UserDetail;
            }
            SetStatus("Vibepollo installation completed with warnings.", warningDetail, _statusWarningBrush);
            await ShowInstallPartialSuccessDialogAsync(result);
            Close();
            return;
          }
          var detail = result.ExitCode == 3010
            ? "The operation completed and Windows restart is required."
            : "All selected changes were applied successfully.";
          if (!string.IsNullOrWhiteSpace(result.UserDetail)) {
            detail += "\n" + result.UserDetail;
          }
          SetStatus(successText, detail, _statusSuccessBrush);
          await ShowOverlayInfoAsync("Complete", _statusText.Text);
          Close();
          return;
        }

        if (result.ExitCode == 1223) {
          ProcessExitCode = result.ExitCode;
          SetStatus(
            actionLabel + " cancelled.",
            "No changes were made because the elevation prompt was cancelled.",
            _statusNormalBrush);
          return;
        }

        if (result.Operation == InstallerOperation.Uninstall && result.ExitCode == 1605) {
          ProcessExitCode = 0;
          SetStatus(
            "Vibepollo is not installed.",
            "Nothing needed to be removed.",
            _statusNormalBrush);
          await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
          return;
        }

        ProcessExitCode = result.ExitCode;
        var failureDetail = BuildFailureDetail(result.Message);
        SetStatus(actionLabel + " failed.", failureDetail, _statusErrorBrush);
        if (result.Operation == InstallerOperation.Install) {
          await ShowInstallFailureSupportDialogAsync(failureDetail, result);
        } else {
          await ShowOverlayInfoAsync("Installer error", failureDetail);
        }
      } catch (Exception ex) {
        ProcessExitCode = 1;
        exceptionFailureDetail = BuildFailureDetail(ex.Message);
        SetStatus(actionLabel + " failed.", exceptionFailureDetail, _statusErrorBrush);
      } finally {
        SetBusyState(false);
      }

      if (!string.IsNullOrWhiteSpace(exceptionFailureDetail)) {
        var failedOperation = string.Equals(actionLabel, "Install", StringComparison.OrdinalIgnoreCase)
          ? InstallerOperation.Install
          : InstallerOperation.Uninstall;
        if (failedOperation == InstallerOperation.Install) {
          await ShowInstallFailureSupportDialogAsync(exceptionFailureDetail, new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = 1,
            Message = exceptionFailureDetail
          });
        } else {
          await ShowOverlayInfoAsync("Installer error", exceptionFailureDetail);
        }
      }
    }

    private static string BuildComponentFailureDetail(IReadOnlyList<string> componentFailures) {
      if (componentFailures == null || componentFailures.Count == 0) {
        return "The operation completed with warnings.";
      }

      var lines = new List<string> {
        "The operation completed, but some components failed:"
      };
      foreach (var failure in componentFailures.Where(item => !string.IsNullOrWhiteSpace(item))) {
        lines.Add("- " + failure.Trim());
      }
      return string.Join("\n", lines);
    }

    private static string NormalizeInstallPath(string installPath) {
      try {
        return InstallerRunner.NormalizeInstallPath(installPath);
      } catch (InvalidOperationException) {
        throw;
      } catch (Exception ex) {
        throw new InvalidOperationException(
          "The install folder path is invalid. Choose a subdirectory under 64-bit Program Files.",
          ex);
      }
    }

    /// <summary>
    /// Checks available disk space and returns a warning message, or null if space is sufficient.
    /// </summary>
    private static string CheckDiskSpace(string installPath) {
      const long MinimumBytesRequired = 500L * 1024 * 1024; // 500 MB
      try {
        var root = Path.GetPathRoot(installPath);
        if (string.IsNullOrEmpty(root)) return null;
        var driveInfo = new DriveInfo(root);
        if (driveInfo.IsReady && driveInfo.AvailableFreeSpace < MinimumBytesRequired) {
          var availableMb = driveInfo.AvailableFreeSpace / (1024 * 1024);
          return "Warning: Drive " + root.TrimEnd('\\') + " has only " + availableMb + " MB free. "
            + "At least 500 MB is recommended. The installation may fail if there is not enough space.";
        }
      } catch {
        // Non-fatal — skip the check if drive info is unavailable
      }
      return null;
    }

    private static string BuildFailureDetail(string message) {
      if (!string.IsNullOrWhiteSpace(message)) {
        return message;
      }
      return "The operation did not complete as expected. Check the MSI log in " + Path.GetTempPath() + " for details, or try running the installer as Administrator.";
    }

    private static string BuildVibeshineInstallWarning(InstallerRunner.InstalledProductInfo vibeshineProduct) {
      var versionSuffix = vibeshineProduct != null && vibeshineProduct.Version != null
        ? " (v" + vibeshineProduct.Version.ToString(3) + ")"
        : string.Empty;

      return "Vibeshine" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibepollo does not carry over Vibeshine settings.\n"
        + "If you intend to stay in the Sunshine ecosystem, Vibeshine is recommended instead.\n\n"
        + "If this is intentional, continue with Vibepollo.\n"
        + "Continuing will uninstall Vibeshine before installation.";
    }

    private static string BuildApolloInstallWarning(InstallerRunner.InstalledProductInfo apolloProduct) {
      var versionSuffix = apolloProduct != null && apolloProduct.Version != null
        ? " (v" + apolloProduct.Version.ToString(3) + ")"
        : string.Empty;

      return "Apollo" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibepollo replaces Apollo and cannot be installed while Apollo is installed.\n"
        + "Continuing will uninstall Apollo before installation.\n\n"
        + "Click Uninstall Apollo to proceed.";
    }

    private string BuildLegacySunshineMigrationWarning() {
      var versionSuffix = string.Empty;
      if (_legacySunshineProduct != null && _legacySunshineProduct.Version != null) {
        versionSuffix = " (v" + _legacySunshineProduct.Version.ToString(3) + ")";
      } else if (!string.IsNullOrWhiteSpace(_legacySunshineRegistration == null ? null : _legacySunshineRegistration.DisplayVersion)) {
        versionSuffix = " (v" + _legacySunshineRegistration.DisplayVersion + ")";
      }

      return "Legacy Sunshine" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibepollo replaces Sunshine. The bootstrapper will uninstall Sunshine first, then start the installation.\n"
        + "No settings will be lost during this migration.\n\n"
        + "Click Uninstall Sunshine to proceed.";
    }

    private string BuildLegacyApolloMigrationWarning() {
      var versionSuffix = string.Empty;
      if (_legacyApolloRegistration != null && !string.IsNullOrWhiteSpace(_legacyApolloRegistration.DisplayVersion)) {
        versionSuffix = " (v" + _legacyApolloRegistration.DisplayVersion + ")";
      }

      return "Legacy Apollo" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibepollo replaces legacy Apollo and will automatically uninstall it first, then install Vibepollo.\n"
        + "No settings will be carried over.\n\n"
        + "Click Uninstall Apollo to proceed.";
    }

    private enum InstallActionKind {
      Install,
      Upgrade,
      Downgrade,
      Reinstall
    }

    private InstallActionKind GetInstallActionKind() {
      if (_installedProduct == null) {
        return InstallActionKind.Install;
      }

      var targetVersion = _payloadMsiInfo == null ? _bundleVersion : _payloadMsiInfo.Version;
      if (_installedProduct.Version != null && targetVersion != null && _installedProduct.Version > targetVersion) {
        return InstallActionKind.Downgrade;
      }

      if (_installedProduct.Version != null && targetVersion != null && _installedProduct.Version < targetVersion) {
        return InstallActionKind.Upgrade;
      }

      var sameVersionAction = GetSameVersionProductCodeAction();
      if (sameVersionAction.HasValue) {
        return sameVersionAction.Value;
      }

      return InstallActionKind.Reinstall;
    }

    private InstallActionKind? GetSameVersionProductCodeAction() {
      if (_installedProduct == null || _payloadMsiInfo == null) {
        return null;
      }
      if (string.IsNullOrWhiteSpace(_installedProduct.ProductCode) || string.IsNullOrWhiteSpace(_payloadMsiInfo.ProductCode)) {
        return null;
      }

      string installedProductCode;
      string payloadProductCode;
      if (!InstallerRunner.TryNormalizeSortableProductCode(_installedProduct.ProductCode, out installedProductCode)
        || !InstallerRunner.TryNormalizeSortableProductCode(_payloadMsiInfo.ProductCode, out payloadProductCode)) {
        return null;
      }

      var comparison = string.CompareOrdinal(payloadProductCode, installedProductCode);
      if (comparison > 0) {
        return InstallActionKind.Upgrade;
      }
      if (comparison < 0) {
        return InstallActionKind.Downgrade;
      }

      return InstallActionKind.Reinstall;
    }

    private string GetTargetVersionText() {
      if (_payloadMsiInfo != null && !string.IsNullOrWhiteSpace(_payloadMsiInfo.VersionText)) {
        return _payloadMsiInfo.VersionText;
      }
      return _bundleVersion.ToString(3);
    }

    private string BuildInstallButtonLabel() {
      switch (GetInstallActionKind()) {
        case InstallActionKind.Install:
          return "Install Vibepollo";
        case InstallActionKind.Upgrade:
          return "Upgrade Vibepollo";
        case InstallActionKind.Downgrade:
          return "Downgrade Vibepollo";
        default:
          return "Reinstall Vibepollo";
      }
    }

    private string ResolvePreferredInstallDirectory() {
      var candidates = new[] {
        _installedProduct == null ? null : _installedProduct.InstallLocation,
        _legacySunshineProduct == null ? null : _legacySunshineProduct.InstallLocation,
        _legacySunshineRegistration == null ? null : _legacySunshineRegistration.InstallLocation,
        _legacyApolloRegistration == null ? null : _legacyApolloRegistration.InstallLocation
      };

      foreach (var candidate in candidates) {
        if (!string.IsNullOrWhiteSpace(candidate)) {
          return candidate;
        }
      }

      return InstallerRunner.DefaultInstallDirectory;
    }

    private static bool IsSudoVdaSelectedInConfiguration(string installDirectory) {
      return InstallerRunner.IsSudoVdaSelectedInConfiguration(installDirectory);
    }

    private async Task ShowLicenseDialogAsync() {
      var maxTextHeight = ActualHeight - 320;
      if (maxTextHeight < 140) {
        maxTextHeight = 140;
      }
      if (maxTextHeight > 230) {
        maxTextHeight = 230;
      }

      await ShowOverlayAsync(
        "License",
        "Vibepollo software license terms:",
        "Close",
        string.Empty,
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        false,
        content => {
          var licenseTextBox = new TextBox {
            Text = _licenseText,
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            MinHeight = maxTextHeight,
            MaxHeight = maxTextHeight,
            Background = new SolidColorBrush(Color.FromRgb(8, 14, 28)),
            Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
            Margin = new Thickness(0, 0, 0, 4),
            Padding = new Thickness(12)
          };
          content.Children.Add(licenseTextBox);
        },
        0);
    }

    private static string LoadEmbeddedLicenseText() {
      const string fallbackText = "License text is unavailable in this installer build.";
      try {
        var assembly = Assembly.GetExecutingAssembly();
        using (var stream = assembly.GetManifestResourceStream("License.txt")) {
          if (stream == null) {
            return fallbackText;
          }

          using (var reader = new StreamReader(stream)) {
            var content = reader.ReadToEnd();
            if (string.IsNullOrWhiteSpace(content)) {
              return fallbackText;
            }
            return content;
          }
        }
      } catch {
        return fallbackText;
      }
    }

    private void SetBusyState(bool busy) {
      _isBusy = busy;
      UpdateActionUiState();
      _continueButton.IsEnabled = !busy;
      _uninstallButton.IsEnabled = !busy && _installedProduct != null;
      _licenseButton.IsEnabled = !busy;
      _closeButton.IsEnabled = !busy;
      _titleCloseButton.IsEnabled = !busy;
      _titleCloseButton.Opacity = busy ? 0.6 : 1.0;
      _progressBar.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateActionUiState() {
      if (BuildFlavor.IsUninstallOnly) {
        var allowUninstall = !_isBusy && _installedProduct != null;
        _installPathTextBox.IsEnabled = false;
        _virtualDisplayDriverComboBox.IsEnabled = false;
        _browseButton.IsEnabled = false;
        _installSection.Visibility = Visibility.Collapsed;
        _installVirtualDisplaySection.Visibility = Visibility.Collapsed;
        _continueButton.Visibility = Visibility.Collapsed;
        _uninstallButton.Visibility = Visibility.Visible;
        _uninstallButton.IsEnabled = allowUninstall;
        return;
      }

      var allowInstallInputs = !_isBusy;
      var hasInstalledProduct = _installedProduct != null;
      var showInstallLocation = !hasInstalledProduct;
      _installLocationTitleText.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;
      _installLocationHintText.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;
      _installPathGrid.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;
      _installPathTextBox.IsEnabled = allowInstallInputs && showInstallLocation;
      _virtualDisplayDriverComboBox.IsEnabled = allowInstallInputs && _showInstallVirtualDisplayOption;
      _browseButton.IsEnabled = allowInstallInputs && showInstallLocation;
      _installSection.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;
      _installVirtualDisplaySection.Visibility = _showInstallVirtualDisplayOption ? Visibility.Visible : Visibility.Collapsed;
      _uninstallButton.Visibility = hasInstalledProduct ? Visibility.Visible : Visibility.Collapsed;
      _continueButton.Visibility = Visibility.Visible;
      _continueButton.Content = BuildInstallButtonLabel();
    }

    private void ResolveOverlay(string result) {
      StopOverlayAutoClose();
      var tcs = _overlayTcs;
      if (tcs == null || tcs.Task.IsCompleted) {
        return;
      }
      _overlayGrid.Visibility = Visibility.Collapsed;
      _overlayTcs = null;
      tcs.TrySetResult(result);
    }

    private Task<string> ShowOverlayAsync(
      string title,
      string message,
      string primaryText,
      string secondaryText,
      Brush primaryBackground,
      Brush primaryBorder,
      bool showSecondary,
      Action<StackPanel> buildContent,
      int autoCloseSeconds) {
      if (_overlayTcs != null && !_overlayTcs.Task.IsCompleted) {
        _overlayTcs.TrySetResult("secondary");
      }
      StopOverlayAutoClose();

      _overlayTitleText.Text = title ?? string.Empty;
      _overlayMessageText.Text = message ?? string.Empty;
      _overlayAccentBar.Background = primaryBackground ?? new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _overlayContentHost.Children.Clear();
      if (buildContent != null) {
        buildContent(_overlayContentHost);
      }

      _overlayPrimaryButton.Content = primaryText ?? "OK";
      _overlayPrimaryButton.Background = primaryBackground ?? new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _overlayPrimaryButton.BorderBrush = primaryBorder ?? new SolidColorBrush(Color.FromRgb(165, 180, 252));

      _overlaySecondaryButton.Content = secondaryText ?? "Cancel";
      _overlaySecondaryButton.Visibility = showSecondary ? Visibility.Visible : Visibility.Collapsed;
      if (autoCloseSeconds > 0 && !showSecondary) {
        _overlayHintText.Visibility = Visibility.Visible;
        _overlayAutoCloseProgressBar.Visibility = Visibility.Visible;
        StartOverlayAutoClose(autoCloseSeconds);
      } else {
        _overlayHintText.Text = string.Empty;
        _overlayHintText.Visibility = Visibility.Collapsed;
        _overlayAutoCloseProgressBar.Visibility = Visibility.Collapsed;
        _overlayAutoCloseProgressBar.Value = 0;
      }

      _overlayGrid.Visibility = Visibility.Visible;
      _overlayPrimaryButton.Focus();

      _overlayTcs = new TaskCompletionSource<string>();
      return _overlayTcs.Task;
    }

    private void StartOverlayAutoClose(int seconds) {
      _overlayAutoCloseSecondsRemaining = seconds <= 0 ? 0 : seconds;
      if (_overlayAutoCloseSecondsRemaining <= 0) {
        return;
      }

      _overlayAutoCloseDeadlineUtc = DateTime.UtcNow.AddSeconds(_overlayAutoCloseSecondsRemaining);
      _overlayAutoCloseTotalSeconds = _overlayAutoCloseSecondsRemaining;
      _overlayAutoCloseProgressBar.Minimum = 0;
      _overlayAutoCloseProgressBar.Maximum = _overlayAutoCloseTotalSeconds;
      _overlayAutoCloseProgressBar.Value = 0;
      UpdateOverlayAutoCloseCountdownUi();
      _overlayAutoCloseTimer = new DispatcherTimer {
        Interval = TimeSpan.FromMilliseconds(100)
      };
      _overlayAutoCloseTimer.Tick += OverlayAutoCloseTimerTick;
      _overlayAutoCloseTimer.Start();
    }

    private void StopOverlayAutoClose() {
      if (_overlayAutoCloseTimer == null) {
        return;
      }
      _overlayAutoCloseTimer.Stop();
      _overlayAutoCloseTimer.Tick -= OverlayAutoCloseTimerTick;
      _overlayAutoCloseTimer = null;
      _overlayAutoCloseSecondsRemaining = 0;
      _overlayAutoCloseTotalSeconds = 0;
      _overlayAutoCloseProgressBar.Value = 0;
    }

    private void OverlayAutoCloseTimerTick(object sender, EventArgs e) {
      if (UpdateOverlayAutoCloseCountdownUi()) {
        ResolveOverlay("primary");
      }
    }

    private bool UpdateOverlayAutoCloseCountdownUi() {
      var secondsRemaining = (_overlayAutoCloseDeadlineUtc - DateTime.UtcNow).TotalSeconds;
      if (secondsRemaining < 0) {
        secondsRemaining = 0;
      }

      var secondsElapsed = _overlayAutoCloseTotalSeconds - secondsRemaining;
      if (secondsElapsed < 0) {
        secondsElapsed = 0;
      }
      if (secondsElapsed > _overlayAutoCloseProgressBar.Maximum) {
        secondsElapsed = _overlayAutoCloseProgressBar.Maximum;
      }
      _overlayAutoCloseProgressBar.Value = secondsElapsed;
      var displaySeconds = (int)Math.Ceiling(secondsRemaining);
      if (displaySeconds <= 0) {
        _overlayHintText.Text = "Closing…";
        return true;
      }

      _overlayHintText.Text = "This message closes automatically in " + displaySeconds + " seconds.";
      return false;
    }

    private async Task<bool> ShowOverlayConfirmAsync(string title, string message, string confirmText, string cancelText, bool destructive) {
      var primaryBg = destructive
        ? (Brush)new SolidColorBrush(Color.FromRgb(225, 29, 72))
        : new SolidColorBrush(Color.FromRgb(99, 102, 241));
      var primaryBorder = destructive
        ? (Brush)new SolidColorBrush(Color.FromRgb(251, 113, 133))
        : new SolidColorBrush(Color.FromRgb(165, 180, 252));
      var result = await ShowOverlayAsync(title, message, confirmText, cancelText, primaryBg, primaryBorder, true, null, 0);
      return string.Equals(result, "primary", StringComparison.OrdinalIgnoreCase);
    }

    private async Task ShowOverlayInfoAsync(string title, string message) {
      await ShowOverlayAsync(
        title,
        message,
        "OK",
        string.Empty,
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        false,
        null,
        5);
    }

    private struct UninstallOptions {
      public bool RemoveVirtualDisplayDriver;
      public bool FactoryResetAppData;
    }

    private async Task<UninstallOptions?> ShowOverlayUninstallOptionsAsync() {
      var removeDriverCheckBox = new CheckBox {
        Content = "Remove virtual display driver",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 0, 0, 8),
        IsChecked = false
      };
      var deleteFolderCheckBox = new CheckBox {
        Content = "Factory reset (deletes Vibepollo settings, preserves user-added files)",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 0, 0, 0),
        IsChecked = false
      };

      var message = "Choose what to remove during uninstall.\n\n"
        + "Uninstall always removes the Vibepollo service, firewall rules, and MSI-installed program files. "
        + "Files you added after installation are preserved.";

      var result = await ShowOverlayAsync(
        "Uninstall Vibepollo",
        message,
        "Uninstall",
        "Cancel",
        new SolidColorBrush(Color.FromRgb(225, 29, 72)),
        new SolidColorBrush(Color.FromRgb(251, 113, 133)),
        true,
        content => {
          content.Children.Add(removeDriverCheckBox);
          content.Children.Add(deleteFolderCheckBox);
        },
        0);

      if (!string.Equals(result, "primary", StringComparison.OrdinalIgnoreCase)) {
        return null;
      }

      return new UninstallOptions {
        RemoveVirtualDisplayDriver = removeDriverCheckBox.IsChecked == true,
        FactoryResetAppData = deleteFolderCheckBox.IsChecked == true
      };
    }

    private async Task ShowInstallFailureSupportDialogAsync(string failureDetail, InstallerResult installResult) {
      var decision = await ShowOverlayAsync(
        "Install failed",
        failureDetail + "\n\nSave logs now, then report this issue on GitHub or Discord.",
        "Save logs",
        "Not now",
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        true,
        content => {
          content.Children.Add(BuildSupportLinksTextBlock());
        },
        0);

      if (!string.Equals(decision, "primary", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      await SaveInstallFailureSupportBundleAsync(failureDetail, installResult);
    }

    private async Task ShowInstallPartialSuccessDialogAsync(InstallerResult installResult) {
      var warningDetail = BuildComponentFailureDetail(installResult == null ? null : installResult.ComponentFailures);
      var decision = await ShowOverlayAsync(
        "Install completed with warnings",
        warningDetail + "\n\nSave logs now, then report this issue on GitHub or Discord.",
        "Save logs",
        "Not now",
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        true,
        content => {
          content.Children.Add(BuildSupportLinksTextBlock());
        },
        0);

      if (!string.Equals(decision, "primary", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      await SaveInstallWarningSupportBundleAsync(warningDetail, installResult);
    }

    private async Task SaveInstallFailureSupportBundleAsync(string failureDetail, InstallerResult installResult) {
      var timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");
      var saveDialog = new SaveFileDialog {
        Title = "Save support logs",
        Filter = "Log report (*.txt)|*.txt",
        DefaultExt = ".txt",
        AddExtension = true,
        OverwritePrompt = true,
        FileName = "vibeshine-install-logs-" + timestamp + ".txt",
        InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
      };

      var selected = saveDialog.ShowDialog(this);
      if (selected != true || string.IsNullOrWhiteSpace(saveDialog.FileName)) {
        return;
      }

      string error = null;
      var outputPath = saveDialog.FileName;
      try {
        await Task.Run(() => WriteInstallFailureSupportReport(outputPath, failureDetail, installResult));
      } catch (Exception ex) {
        error = ex.Message;
      }

      if (!string.IsNullOrWhiteSpace(error)) {
        SetStatus("Could not save support logs.", error, _statusErrorBrush);
        await ShowOverlayInfoAsync("Could not save logs", error);
        return;
      }

      var nextStep = "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues\n"
        + "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN";
      SetStatus("Support logs saved.", outputPath, _statusSuccessBrush);
      await ShowOverlayInfoAsync(
        "Logs saved",
        "Saved support logs to:\n" + outputPath + "\n\n" + nextStep);
    }

    private async Task SaveInstallWarningSupportBundleAsync(string warningDetail, InstallerResult installResult) {
      var timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");
      var saveDialog = new SaveFileDialog {
        Title = "Save support logs",
        Filter = "Log report (*.txt)|*.txt",
        DefaultExt = ".txt",
        AddExtension = true,
        OverwritePrompt = true,
        FileName = "vibeshine-install-warnings-" + timestamp + ".txt",
        InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
      };

      var selected = saveDialog.ShowDialog(this);
      if (selected != true || string.IsNullOrWhiteSpace(saveDialog.FileName)) {
        return;
      }

      string error = null;
      var outputPath = saveDialog.FileName;
      try {
        await Task.Run(() => WriteInstallWarningSupportReport(outputPath, warningDetail, installResult));
      } catch (Exception ex) {
        error = ex.Message;
      }

      if (!string.IsNullOrWhiteSpace(error)) {
        SetStatus("Could not save support logs.", error, _statusErrorBrush);
        await ShowOverlayInfoAsync("Could not save logs", error);
        return;
      }

      var nextStep = "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues\n"
        + "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN";
      SetStatus("Support logs saved.", outputPath, _statusSuccessBrush);
      await ShowOverlayInfoAsync(
        "Logs saved",
        "Saved support logs to:\n" + outputPath + "\n\n" + nextStep);
    }

    private void WriteInstallFailureSupportReport(string outputPath, string failureDetail, InstallerResult installResult) {
      var candidateLogs = CollectSupportLogFiles(installResult == null ? null : installResult.LogPath);
      var destination = "GitHub issue or Discord #vibeshine";
      var executionVersion = _bundleVersion.ToString(3);
      using (var writer = new StreamWriter(outputPath, false)) {
        writer.WriteLine(BuildSupportSummary(destination, executionVersion, failureDetail, installResult, candidateLogs.Count, "Vibepollo install failure report", "Failure detail:"));
        writer.WriteLine();

        if (candidateLogs.Count == 0) {
          writer.WriteLine("No installer logs were found.");
          return;
        }

        foreach (var file in candidateLogs) {
          writer.WriteLine("===== BEGIN LOG: " + file + " =====");
          try {
            foreach (var line in File.ReadLines(file)) {
              writer.WriteLine(line);
            }
          } catch (Exception ex) {
            writer.WriteLine("[Could not read this log file: " + ex.Message + "]");
          }
          writer.WriteLine("===== END LOG: " + file + " =====");
          writer.WriteLine();
        }
      }
    }

    private void WriteInstallWarningSupportReport(string outputPath, string warningDetail, InstallerResult installResult) {
      var candidateLogs = CollectSupportLogFiles(installResult == null ? null : installResult.LogPath);
      var destination = "GitHub issue or Discord #vibeshine";
      var executionVersion = _bundleVersion.ToString(3);
      using (var writer = new StreamWriter(outputPath, false)) {
        writer.WriteLine(BuildSupportSummary(destination, executionVersion, warningDetail, installResult, candidateLogs.Count, "Vibepollo install warning report", "Warning detail:"));
        writer.WriteLine();

        if (candidateLogs.Count == 0) {
          writer.WriteLine("No installer logs were found.");
          return;
        }

        foreach (var file in candidateLogs) {
          writer.WriteLine("===== BEGIN LOG: " + file + " =====");
          try {
            foreach (var line in File.ReadLines(file)) {
              writer.WriteLine(line);
            }
          } catch (Exception ex) {
            writer.WriteLine("[Could not read this log file: " + ex.Message + "]");
          }
          writer.WriteLine("===== END LOG: " + file + " =====");
          writer.WriteLine();
        }
      }
    }

    private static List<string> CollectSupportLogFiles(string preferredLogPath) {
      var collected = new List<string>();
      var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      TryAddLogFile(collected, seen, preferredLogPath);

      var tempPath = Path.GetTempPath();
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_install_*.log", 8);
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_preinstall_remove_*.log", 8);
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_uninstall_*.log", 4);

      var programFilesLogs = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        "Sunshine",
        "config",
        "logs");
      TryAddRecentLogs(collected, seen, programFilesLogs, "*.log", 8);

      var roamingLogs = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "Sunshine",
        "logs");
      TryAddRecentLogs(collected, seen, roamingLogs, "*.log", 8);

      return collected;
    }

    private static void TryAddRecentLogs(List<string> collected, HashSet<string> seen, string directory, string pattern, int limit) {
      if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) {
        return;
      }

      IEnumerable<string> files;
      try {
        files = Directory
          .GetFiles(directory, pattern, SearchOption.TopDirectoryOnly)
          .OrderByDescending(path => {
            try {
              return File.GetLastWriteTimeUtc(path);
            } catch {
              return DateTime.MinValue;
            }
          })
          .Take(limit);
      } catch {
        return;
      }

      foreach (var file in files) {
        TryAddLogFile(collected, seen, file);
      }
    }

    private static void TryAddLogFile(List<string> collected, HashSet<string> seen, string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return;
      }

      string fullPath;
      try {
        fullPath = Path.GetFullPath(path);
      } catch {
        return;
      }

      if (!File.Exists(fullPath) || seen.Contains(fullPath)) {
        return;
      }

      seen.Add(fullPath);
      collected.Add(fullPath);
    }

    private static string BuildSupportSummary(
      string destination,
      string installerVersion,
      string detail,
      InstallerResult result,
      int collectedLogCount,
      string reportTitle,
      string detailLabel) {
      var lines = new List<string> {
        string.IsNullOrWhiteSpace(reportTitle) ? "Vibepollo install support report" : reportTitle,
        "Generated (UTC): " + DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss"),
        "Destination: " + destination,
        "Installer version: " + installerVersion,
        "Exit code: " + (result == null ? "unknown" : result.ExitCode.ToString()),
        "Operation: " + (result == null ? "install" : result.Operation.ToString()),
        "Collected logs: " + collectedLogCount,
        string.Empty,
        string.IsNullOrWhiteSpace(detailLabel) ? "Detail:" : detailLabel,
        detail ?? "Unknown error",
        string.Empty,
        "Next step:",
        "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues",
        "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN"
      };
      return string.Join(Environment.NewLine, lines);
    }

    private TextBlock BuildSupportLinksTextBlock() {
      var block = new TextBlock {
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        Margin = new Thickness(0, 0, 0, 8),
        TextWrapping = TextWrapping.Wrap
      };

      block.Inlines.Add(new Run("Open an issue on "));
      var githubLink = new Hyperlink(new Run("GitHub")) {
        NavigateUri = new Uri("https://github.com/Nonary/Vibepollo/issues")
      };
      githubLink.Click += (sender, args) => OpenExternalUrl("https://github.com/Nonary/Vibepollo/issues");
      block.Inlines.Add(githubLink);
      block.Inlines.Add(new Run(" or join "));
      var discordLink = new Hyperlink(new Run("Discord (#vibeshine)")) {
        NavigateUri = new Uri("https://discord.com/invite/CGg5JxN")
      };
      discordLink.Click += (sender, args) => OpenExternalUrl("https://discord.com/invite/CGg5JxN");
      block.Inlines.Add(discordLink);
      block.Inlines.Add(new Run("."));

      return block;
    }

    private static void OpenExternalUrl(string url) {
      if (string.IsNullOrWhiteSpace(url)) {
        return;
      }

      try {
        Process.Start(new ProcessStartInfo {
          FileName = url,
          UseShellExecute = true
        });
      } catch {
      }
    }

    private void SetStatus(string headline, string detail, Brush headlineBrush) {
      _statusText.Text = headline;
      _statusText.Foreground = headlineBrush;
      _statusDetailText.Text = detail ?? string.Empty;
    }
  }

  internal enum InstallerOperation {
    Install,
    Uninstall
  }

  internal sealed class InstallerResult {
    public InstallerOperation Operation { get; set; }
    public int ExitCode { get; set; }
    public string Message { get; set; }
    public string UserDetail { get; set; }
    public string LogPath { get; set; }
    public List<string> ComponentFailures { get; set; }
    public string ProductCode { get; set; }
    public string ProductDisplayName { get; set; }
    public InstallerRunner.InstalledProductKind ProductKind { get; set; }
    public bool InstallDeferredForRestart { get; set; }
    public bool Succeeded {
      get { return !InstallDeferredForRestart && (ExitCode == 0 || ExitCode == 3010); }
    }
    public bool PartiallySucceeded {
      get { return Succeeded && ComponentFailures != null && ComponentFailures.Count > 0; }
    }
  }

  internal sealed class InternalInstallResultSnapshot {
    public int ExitCode { get; set; }
    public string Message { get; set; }
    public string UserDetail { get; set; }
    public string LogPath { get; set; }
    public List<string> ComponentFailures { get; set; }
    public bool InstallDeferredForRestart { get; set; }
  }

  internal sealed class InstallerArguments {
    private static readonly string[] HelpTokens = { "/?", "/h", "-h", "--help" };
    private static readonly string[] UiTokens = { "--ui" };
    private static readonly string[] NoUiTokens = { "--no-ui" };
    private static readonly string[] UninstallUiTokens = { "--uninstall-ui", "--uninstall", "/uninstall" };
    private static readonly string[] QuietTokens = { "/quiet", "/qn", "/qb", "/passive" };
    private const string InternalElevatedInstallToken = "--internal-elevated-install";
    private const string InternalElevatedUninstallToken = "--internal-elevated-uninstall";
    private const string InternalInstallPathToken = "--internal-install-path";
    private const string InternalInstallVirtualDisplayDriverToken = "--internal-install-virtual-display-driver";
    private const string InternalInstallSaveLogsToken = "--internal-install-save-logs";
    private const string InternalInstallResultPathToken = "--internal-install-result-path";
    private const string InternalUninstallDeleteInstallDirToken = "--internal-uninstall-delete-install-dir";
    private const string InternalUninstallFactoryResetToken = "--internal-uninstall-factory-reset";
    private const string InternalUninstallRemoveVirtualDisplayDriverToken = "--internal-uninstall-remove-virtual-display-driver";

    public bool ShowUi { get; set; }
    public bool UninstallUiRequested { get; set; }
    public bool InternalElevatedInstall { get; set; }
    public bool InternalElevatedUninstall { get; set; }
    public string InternalInstallPath { get; set; }
    public bool InternalInstallVirtualDisplay { get; set; }
    public bool InternalInstallSaveLogs { get; set; }
    public string InternalInstallResultPath { get; set; }
    public bool InternalUninstallFactoryReset { get; set; }
    public bool InternalUninstallRemoveVirtualDisplayDriver { get; set; }
    public string MsiPathOverride { get; set; }
    public List<string> ForwardedArguments { get; private set; }

    public InstallerArguments() {
      InternalInstallVirtualDisplay = true;
      ForwardedArguments = new List<string>();
    }

    public static bool IsHelpRequested(string[] args) {
      return args.Any(arg => HelpTokens.Contains(arg, StringComparer.OrdinalIgnoreCase));
    }

    public static InstallerArguments Parse(string[] args) {
      var parsed = new InstallerArguments();
      var showUiFlag = false;
      var noUiFlag = false;

      for (var index = 0; index < args.Length; index++) {
        var arg = args[index];
        if (UiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          showUiFlag = true;
          continue;
        }
        if (NoUiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          noUiFlag = true;
          continue;
        }
        if (UninstallUiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          parsed.UninstallUiRequested = true;
          continue;
        }
        if (string.Equals(arg, InternalElevatedInstallToken, StringComparison.OrdinalIgnoreCase)) {
          parsed.InternalElevatedInstall = true;
          continue;
        }
        if (string.Equals(arg, InternalElevatedUninstallToken, StringComparison.OrdinalIgnoreCase)) {
          parsed.InternalElevatedUninstall = true;
          continue;
        }
        if (string.Equals(arg, InternalInstallPathToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallPath = args[++index];
          continue;
        }
        if (string.Equals(arg, InternalInstallVirtualDisplayDriverToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallVirtualDisplay = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalInstallSaveLogsToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallSaveLogs = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalInstallResultPathToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallResultPath = args[++index];
          continue;
        }
        if ((string.Equals(arg, InternalUninstallFactoryResetToken, StringComparison.OrdinalIgnoreCase)
          || string.Equals(arg, InternalUninstallDeleteInstallDirToken, StringComparison.OrdinalIgnoreCase))
          && index + 1 < args.Length) {
          parsed.InternalUninstallFactoryReset = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalUninstallRemoveVirtualDisplayDriverToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalUninstallRemoveVirtualDisplayDriver = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, "--msi", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.MsiPathOverride = args[++index];
          continue;
        }
        parsed.ForwardedArguments.Add(arg);
      }

      if (showUiFlag) {
        parsed.ShowUi = true;
      } else if (noUiFlag) {
        parsed.ShowUi = false;
      } else {
        parsed.ShowUi = parsed.ForwardedArguments.Count == 0;
      }

      return parsed;
    }

    private static bool ParseBooleanToken(string value) {
      return value == "1"
        || value.Equals("true", StringComparison.OrdinalIgnoreCase)
        || value.Equals("yes", StringComparison.OrdinalIgnoreCase);
    }

    public static void WriteHelp() {
#if UNINSTALL_ONLY
      Console.WriteLine("Vibepollo Uninstaller");
      Console.WriteLine("  Self-contained graphical uninstaller for Vibepollo.");
      Console.WriteLine();
      Console.WriteLine("Usage:");
      Console.WriteLine("  uninstall.exe          Launch graphical uninstall UI");
      Console.WriteLine("  uninstall.exe /quiet   Run silent uninstall");
      Console.WriteLine();
      Console.WriteLine("Optional switches forwarded to MSI uninstall:");
      Console.WriteLine("  /quiet, /qn, /qb, /passive");
      Console.WriteLine();
      Console.WriteLine("Examples:");
      Console.WriteLine("  uninstall.exe");
      Console.WriteLine("  uninstall.exe /quiet");
#else
      Console.WriteLine("Vibepollo Installer");
      Console.WriteLine("  Self-hosted game streaming server — stream your PC to any device.");
      Console.WriteLine();
      Console.WriteLine("Usage:");
      Console.WriteLine("  VibepolloSetup.exe                Launch graphical installer UI");
      Console.WriteLine("  VibepolloSetup.exe [MSI options]  Pass options to msiexec");
      Console.WriteLine();
      Console.WriteLine("Wrapper options:");
      Console.WriteLine("  --ui            Force graphical mode (default when no arguments given)");
      Console.WriteLine("  --no-ui         Force command-line passthrough mode");
      Console.WriteLine("  --uninstall-ui  Open graphical UI in uninstall mode");
      Console.WriteLine("  /uninstall      Open graphical UI in uninstall mode (used by ARP)");
      Console.WriteLine("  /?, /h, --help  Show this help message");
      Console.WriteLine();
      Console.WriteLine("Supported MSI properties:");
      Console.WriteLine("  INSTALL_ROOT=<path>  Install under the native 64-bit %ProgramFiles% subtree (default: %ProgramFiles%\\Apollo)");
      Console.WriteLine("  INSTALL_VIRTUAL_DISPLAY_DRIVER=0  Use SudoVDA instead of the default Vibepollo Display Driver");
      Console.WriteLine();
      Console.WriteLine("Examples:");
      Console.WriteLine("  VibepolloSetup.exe /qn");
      Console.WriteLine("  VibepolloSetup.exe /qn INSTALL_ROOT=\"%ProgramFiles%\\Apollo\"");
      Console.WriteLine("  VibepolloSetup.exe /x {PRODUCT-CODE} /qn");
      Console.WriteLine("  VibepolloSetup.exe /qn INSTALL_VIRTUAL_DISPLAY_DRIVER=0");
      Console.WriteLine("  VibepolloSetup.exe /uninstall");
      Console.WriteLine("  VibepolloSetup.exe /uninstall /quiet");
#endif
    }

    public bool IsCliQuietMode() {
      return ForwardedArguments.Any(arg => QuietTokens.Contains(arg, StringComparer.OrdinalIgnoreCase));
    }
  }

  internal static class InstallerRunner {
    private const int MsiExecTimeoutMilliseconds = 30 * 60 * 1000;
    private const int MsiExecTimeoutExitCode = 258;
    // ERROR_FAIL_REBOOT_REQUIRED: cleanup succeeded, but the requested
    // installation itself has not run and must not be reported as successful.
    private const int InstallDeferredRebootRequiredExitCode = 3017;
    private static readonly string[] OperationTokens = {
      "/i",
      "/package",
      "/a",
      "/x",
      "/uninstall",
      "/f",
      "/update"
    };
    private static readonly Version UpgradeSourcePreUninstallVersion = new Version(1, 14, 8);
    private static readonly string[] UninstallRegistryRoots = {
      @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
      @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
    };
    private static readonly RegistryKey[] UninstallRegistryHives = {
      Registry.LocalMachine,
      Registry.CurrentUser
    };
    private static readonly InstalledProductKind[] MsiRegistrationRecoveryKinds = {
      InstalledProductKind.Vibeshine,
      InstalledProductKind.Vibepollo
    };
    private static readonly string[] MsiCacheFailureLogMarkers = {
      "This installation source for this product is not available",
      "The installation source for this product is not available",
      "No valid source could be found for product",
      "Error 1706",
      "failed to resolve source",
      "cached MSI",
      "cached package",
      "source is absent",
      "The configuration data for this product is corrupt",
      "MainEngineThread is returning 1612",
      "MainEngineThread is returning 1610"
    };
    private const string MsiFirewallExceptionUninstallFailureLogMarker =
      "CustomAction WixExecFirewallExceptionsUninstall returned actual error code 1603";
    private static readonly string[] RelatedServiceNames = {
      "ApolloService",
      "SunshineService",
      "VibeshineService",
      "sunshinesvc"
    };
    private static readonly string[] RelatedProcessNames = {
      "sunshine",
      "sunshinesvc",
      "sunshine_wgc_capture",
      "playnite-launcher",
      "playnite_launcher",
      "sunshine_display_helper",
      "sunshine_audio_policy_helper",
      "apollo",
      "apollosvc",
      "vibepollo"
    };

    internal sealed class InstalledProductInfo {
      public string ProductCode { get; set; }
      public string DisplayName { get; set; }
      public Version Version { get; set; }
      public InstalledProductKind Kind { get; set; }
      public bool IsWindowsInstaller { get; set; }
      public bool IsPerUser { get; set; }
      public string InstallLocation { get; set; }
      public string UninstallString { get; set; }
      public string QuietUninstallString { get; set; }
      public string RegistryPath { get; set; }
    }

    internal sealed class PayloadMsiInfo {
      public string ProductCode { get; set; }
      public string UpgradeCode { get; set; }
      public string VersionText { get; set; }
      public Version Version { get; set; }
      public bool SupportsTransactionalReplacement { get; set; }
    }

    // Copy of the currently installed Vibeshine MSI (from the Windows
    // Installer package cache), taken before a legacy uninstall-then-install
    // workaround removes the product, so a failed install phase can restore
    // the previous version instead of leaving nothing installed.
    internal sealed class StashedVibeshinePayload {
      public string MsiPath { get; set; }
      public string ProductCode { get; set; }
      public string InstallLocation { get; set; }
      public string RecoveryDirectory { get; set; }
    }

    internal sealed class LegacySunshineRegistration {
      public string DisplayName { get; set; }
      public string DisplayVersion { get; set; }
      public string UninstallString { get; set; }
      public string QuietUninstallString { get; set; }
      public string InstallLocation { get; set; }
      public string RegistryPath { get; set; }
    }

    private sealed class MsiRegistrationCleanupResult {
      public int TargetCount { get; set; }
      public int RemovedItems { get; set; }
      public List<string> Errors { get; private set; }

      public MsiRegistrationCleanupResult() {
        Errors = new List<string>();
      }

      public bool Succeeded {
        get { return TargetCount > 0 && RemovedItems > 0 && Errors.Count == 0; }
      }
    }

    internal enum InstalledProductKind {
      Unknown,
      Vibeshine,
      Vibepollo,
      Apollo,
      Sunshine
    }

    public static string DefaultInstallDirectory {
      get {
        return Path.Combine(
          GetNativeProgramFilesDirectory(),
          "Apollo");
      }
    }

    private sealed class PinnedMsiPayload : IDisposable {
      public string Path { get; private set; }
      public string Sha256 { get; private set; }
      public string ProductCode { get; private set; }
      public FileStream Stream { get; private set; }
      private readonly List<SafeFileHandle> _ancestorPins;

      public PinnedMsiPayload(
        string path,
        string sha256,
        FileStream stream,
        List<SafeFileHandle> ancestorPins,
        string productCode = null) {
        Path = path;
        Sha256 = sha256;
        ProductCode = NormalizeProductCode(productCode);
        Stream = stream;
        _ancestorPins = ancestorPins ?? new List<SafeFileHandle>();
      }

      public void Dispose() {
        if (Stream != null) {
          Stream.Dispose();
          Stream = null;
        }
        foreach (var pin in _ancestorPins) {
          if (pin != null) {
            pin.Dispose();
          }
        }
        _ancestorPins.Clear();
      }
    }

    private static readonly object ApprovedPayloadLock = new object();
    private static readonly Dictionary<string, string> ApprovedPayloadHashes =
      new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    private static readonly Lazy<string> TrustedInstallerCacheRoot =
      new Lazy<string>(CreateTrustedInstallerCacheRoot, true);

    private static string GetNativeProgramFilesDirectory() {
      var view = Environment.Is64BitOperatingSystem
        ? RegistryView.Registry64
        : RegistryView.Default;
      try {
        using (var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
        using (var currentVersion = machine.OpenSubKey(
          @"SOFTWARE\Microsoft\Windows\CurrentVersion",
          false)) {
          var configured = currentVersion == null
            ? string.Empty
            : Convert.ToString(currentVersion.GetValue(
              "ProgramFilesDir",
              string.Empty,
              RegistryValueOptions.DoNotExpandEnvironmentNames));
          if (!string.IsNullOrWhiteSpace(configured)) {
            return Path.GetFullPath(configured)
              .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
          }
        }
      } catch (Exception ex) {
        if (!(ex is IOException) && !(ex is UnauthorizedAccessException) &&
            !(ex is System.Security.SecurityException)) {
          throw;
        }
      }
      if (!Environment.Is64BitOperatingSystem || Environment.Is64BitProcess) {
        var fallback = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        if (!string.IsNullOrWhiteSpace(fallback)) {
          return Path.GetFullPath(fallback)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
      }
      throw new InvalidOperationException(
        "Windows did not provide the native 64-bit Program Files directory.");
    }

    private static void ValidateInstallPathComponents(string fullPath, string programFiles) {
      var relative = fullPath.Substring(programFiles.Length)
        .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
      var invalid = Path.GetInvalidFileNameChars();
      foreach (var component in relative.Split(
        new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
        StringSplitOptions.RemoveEmptyEntries)) {
        if (component.EndsWith(".", StringComparison.Ordinal) ||
            component.EndsWith(" ", StringComparison.Ordinal) ||
            component.IndexOf('%') >= 0 ||
            component.IndexOfAny(invalid) >= 0 ||
            component.Any(character => char.IsControl(character))) {
          throw new InvalidOperationException(
            "The install folder contains a Windows-reserved path component.");
        }
        var deviceName = component.Split('.')[0].ToUpperInvariant();
        var numberedDevice = deviceName.Length == 4 &&
          (deviceName.StartsWith("COM", StringComparison.Ordinal) ||
           deviceName.StartsWith("LPT", StringComparison.Ordinal)) &&
          deviceName[3] >= '1' && deviceName[3] <= '9';
        if (deviceName == "CON" || deviceName == "PRN" ||
            deviceName == "AUX" || deviceName == "NUL" ||
            deviceName == "CLOCK$" || numberedDevice) {
          throw new InvalidOperationException(
            "The install folder contains a Windows-reserved device name.");
        }
      }
    }

    // The LocalSystem service and its audio-policy helper must execute only
    // from a canonical, non-reparse subtree of the native 64-bit Program Files
    // directory. This is intentionally shared by UI, elevated, direct MSI,
    // quiet, and bootstrapper entry paths.
    internal static string NormalizeInstallPath(string installPath) {
      var trimmedPath = (installPath ?? string.Empty).Trim().Trim('"');
      if (trimmedPath.Length == 0) {
        throw new InvalidOperationException(
          "Choose an install folder before clicking Install or Update.");
      }

      string fullPath;
      try {
        fullPath = Path.GetFullPath(trimmedPath)
          .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
      } catch (Exception ex) {
        if (ex is ArgumentException || ex is NotSupportedException || ex is PathTooLongException) {
          throw new InvalidOperationException(
            "The install folder path is invalid. Choose a subdirectory under 64-bit Program Files.",
            ex);
        }
        throw;
      }

      var programFiles = GetNativeProgramFilesDirectory();
      var requiredPrefix = programFiles + Path.DirectorySeparatorChar;
      if (string.Equals(fullPath, programFiles, StringComparison.OrdinalIgnoreCase) ||
          !fullPath.StartsWith(requiredPrefix, StringComparison.OrdinalIgnoreCase)) {
        throw new InvalidOperationException(
          "Vibepollo must be installed in a subdirectory of the native 64-bit Program Files directory.");
      }
      ValidateInstallPathComponents(fullPath, programFiles);

      var current = fullPath;
      for (;;) {
        if (Directory.Exists(current) || File.Exists(current)) {
          try {
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) {
              throw new InvalidOperationException(
                "The install folder or one of its existing ancestors is a reparse point.");
            }
          } catch (FileNotFoundException) {
          } catch (DirectoryNotFoundException) {
          }
        }
        if (string.Equals(current, programFiles, StringComparison.OrdinalIgnoreCase)) {
          break;
        }
        var parent = Directory.GetParent(current);
        if (parent == null) {
          throw new InvalidOperationException(
            "The install folder is not beneath the native 64-bit Program Files directory.");
        }
        current = parent.FullName.TrimEnd(
          Path.DirectorySeparatorChar,
          Path.AltDirectorySeparatorChar);
      }
      return fullPath;
    }

    public static bool IsSunshineVirtualDisplayDriverEnabledInConfiguration(string installDirectory) {
      foreach (var configPath in BuildSunshineConfigPathCandidates(installDirectory)) {
        bool enabled;
        if (TryReadSunshineVirtualDisplayDriverEnabled(configPath, out enabled)) {
          return enabled;
        }
      }

      return false;
    }

    public static bool IsSudoVdaSelectedInConfiguration(string installDirectory) {
      foreach (var configPath in BuildSunshineConfigPathCandidates(installDirectory)) {
        bool enabled;
        if (TryReadSunshineVirtualDisplayDriverEnabled(configPath, out enabled)) {
          return !enabled;
        }
      }

      return false;
    }

    private static bool TryReadSunshineVirtualDisplayDriverEnabledInConfiguration(string installDirectory, out bool enabled) {
      foreach (var configPath in BuildSunshineConfigPathCandidates(installDirectory)) {
        if (TryReadSunshineVirtualDisplayDriverEnabled(configPath, out enabled)) {
          return true;
        }
      }

      enabled = false;
      return false;
    }

    private static IEnumerable<string> BuildSunshineConfigPathCandidates(string installDirectory) {
      if (!string.IsNullOrWhiteSpace(installDirectory)) {
        yield return Path.Combine(installDirectory, "config", "sunshine.conf");
        yield return Path.Combine(installDirectory, "sunshine.conf");
      }

      var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
      if (!string.IsNullOrWhiteSpace(roaming)) {
        yield return Path.Combine(roaming, "Sunshine", "sunshine.conf");
        yield return Path.Combine(roaming, "Sunshine", "config", "sunshine.conf");
      }
    }

    private static bool TryReadSunshineVirtualDisplayDriverEnabled(string configPath, out bool enabled) {
      enabled = false;
      if (string.IsNullOrWhiteSpace(configPath) || !File.Exists(configPath)) {
        return false;
      }

      try {
        foreach (var rawLine in File.ReadLines(configPath)) {
          var line = rawLine.Trim();
          if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal) || line.StartsWith(";", StringComparison.Ordinal)) {
            continue;
          }

          var equalsIndex = line.IndexOf('=');
          if (equalsIndex <= 0) {
            continue;
          }

          var key = line.Substring(0, equalsIndex).Trim();
          if (!string.Equals(key, "dd_use_sunshine_virtual_display_driver", StringComparison.OrdinalIgnoreCase)) {
            continue;
          }

          var value = line.Substring(equalsIndex + 1).Trim().Trim('"');
          enabled = IsTruthyConfigValue(value);
          return true;
        }
      } catch {
        return false;
      }

      return false;
    }

    private static bool IsTruthyConfigValue(string value) {
      return string.Equals(value, "true", StringComparison.OrdinalIgnoreCase)
        || string.Equals(value, "1", StringComparison.OrdinalIgnoreCase)
        || string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase)
        || string.Equals(value, "on", StringComparison.OrdinalIgnoreCase)
        || string.Equals(value, "enabled", StringComparison.OrdinalIgnoreCase);
    }

    public static InstalledProductInfo GetInstalledVibeshineProduct() {
      return GetInstalledProducts(false)
        .Where(product => product.Kind == InstalledProductKind.Vibeshine)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static InstalledProductInfo GetInstalledVibepolloProduct() {
      return GetInstalledProducts(false)
        .Where(product => product.Kind == InstalledProductKind.Vibepollo)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static InstalledProductInfo GetInstalledApolloProduct() {
      return GetInstalledProducts(true)
        .Where(product => product.Kind == InstalledProductKind.Apollo)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static List<InstalledProductInfo> GetInstalledApolloFamilyProducts() {
      return GetInstalledProductRegistrations(true)
        .Where(product => product.Kind == InstalledProductKind.Apollo || product.Kind == InstalledProductKind.Vibepollo)
        .GroupBy(BuildProductRegistrationIdentity, StringComparer.OrdinalIgnoreCase)
        .Select(MergeInstalledProductGroup)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ToList();
    }

    public static InstalledProductInfo GetInstalledSunshineProduct() {
      var msiProduct = GetInstalledProducts(true)
        .Where(product => product.Kind == InstalledProductKind.Sunshine)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();

      var registration = GetInstalledProductRegistrations(true)
        .Where(product => product.Kind == InstalledProductKind.Sunshine)
        .Where(CanUninstallProduct)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ThenByDescending(product => string.IsNullOrWhiteSpace(product.InstallLocation) ? 0 : 1)
        .FirstOrDefault();

      if (msiProduct != null) {
        MergeProductMetadata(msiProduct, registration);
        return msiProduct;
      }

      return registration;
    }

    public static LegacySunshineRegistration GetLegacySunshineRegistration() {
      var registration = GetInstalledProductRegistrations(true)
        .Where(product => product.Kind == InstalledProductKind.Sunshine)
        .Where(CanUninstallProduct)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ThenByDescending(product => string.IsNullOrWhiteSpace(product.InstallLocation) ? 0 : 1)
        .FirstOrDefault();

      if (registration == null) {
        return null;
      }

      return new LegacySunshineRegistration {
        DisplayName = string.IsNullOrWhiteSpace(registration.DisplayName) ? "Sunshine" : registration.DisplayName,
        DisplayVersion = registration.Version == null ? string.Empty : registration.Version.ToString(),
        UninstallString = registration.UninstallString ?? string.Empty,
        QuietUninstallString = registration.QuietUninstallString ?? string.Empty,
        InstallLocation = registration.InstallLocation ?? string.Empty,
        RegistryPath = registration.RegistryPath ?? string.Empty
      };
    }

    public static LegacySunshineRegistration GetLegacyApolloRegistration() {
      var registration = GetInstalledProductRegistrations(true)
        .Where(product => product.Kind == InstalledProductKind.Apollo)
        .Where(CanUninstallProduct)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ThenByDescending(product => string.IsNullOrWhiteSpace(product.InstallLocation) ? 0 : 1)
        .FirstOrDefault();

      if (registration == null) {
        return null;
      }

      return new LegacySunshineRegistration {
        DisplayName = string.IsNullOrWhiteSpace(registration.DisplayName) ? "Apollo" : registration.DisplayName,
        DisplayVersion = registration.Version == null ? string.Empty : registration.Version.ToString(),
        UninstallString = registration.UninstallString ?? string.Empty,
        QuietUninstallString = registration.QuietUninstallString ?? string.Empty,
        InstallLocation = registration.InstallLocation ?? string.Empty,
        RegistryPath = registration.RegistryPath ?? string.Empty
      };
    }

    public static PayloadMsiInfo TryGetPayloadMsiInfo(InstallerArguments arguments) {
      try {
        var msiPath = ResolveMsiPath(arguments == null ? null : arguments.MsiPathOverride);
        return TryGetPayloadMsiInfo(msiPath);
      } catch {
        return null;
      }
    }

    private static PayloadMsiInfo TryGetPayloadMsiInfo(string msiPath) {
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return null;
      }

      IntPtr packageHandle;
      var openCode = MsiOpenPackageEx(msiPath, MsiOpenPackageIgnoreMachineState, out packageHandle);
      if (openCode != MsiErrorSuccess || packageHandle == IntPtr.Zero) {
        return null;
      }

      try {
        var productCode = ReadMsiProperty(packageHandle, "ProductCode");
        var upgradeCode = ReadMsiProperty(packageHandle, "UpgradeCode");
        var versionText = ReadMsiProperty(packageHandle, "ProductVersion");
        var transactionalReplacement = ReadMsiProperty(packageHandle, "VIBESHINE_TRANSACTIONAL_REPLACEMENT");
        if (string.IsNullOrWhiteSpace(productCode) && string.IsNullOrWhiteSpace(versionText) && string.IsNullOrWhiteSpace(upgradeCode)) {
          return null;
        }

        return new PayloadMsiInfo {
          ProductCode = productCode ?? string.Empty,
          UpgradeCode = upgradeCode ?? string.Empty,
          VersionText = versionText ?? string.Empty,
          Version = ParseVersion(versionText),
          SupportsTransactionalReplacement = string.Equals(transactionalReplacement, "1", StringComparison.Ordinal)
        };
      } finally {
        MsiCloseHandle(packageHandle);
      }
    }

    private static string ReadMsiProperty(IntPtr packageHandle, string propertyName) {
      if (packageHandle == IntPtr.Zero || string.IsNullOrWhiteSpace(propertyName)) {
        return null;
      }

      uint length = 256;
      var buffer = new StringBuilder((int)length);
      var getCode = MsiGetProperty(packageHandle, propertyName, buffer, ref length);
      if (getCode == MsiErrorMoreData) {
        length += 1;
        buffer = new StringBuilder((int)length);
        getCode = MsiGetProperty(packageHandle, propertyName, buffer, ref length);
      }
      if (getCode != MsiErrorSuccess) {
        return string.Empty;
      }

      return buffer.ToString();
    }

    private static List<InstalledProductInfo> GetInstalledProducts(bool includeSunshine) {
      var installedProducts = new List<InstalledProductInfo>();
      foreach (var hive in UninstallRegistryHives) {
        foreach (var root in UninstallRegistryRoots) {
          using (var uninstallRoot = hive.OpenSubKey(root)) {
            if (uninstallRoot != null) {
              CollectProductsFromRoot(hive, uninstallRoot, root, installedProducts, includeSunshine);
            }
          }
        }
      }

      return installedProducts
        .GroupBy(product => product.ProductCode, StringComparer.OrdinalIgnoreCase)
        .Select(group => group.OrderByDescending(item => item.Version ?? new Version(0, 0, 0, 0)).First())
        .ToList();
    }

    private static List<InstalledProductInfo> GetInstalledProductRegistrations(bool includeSunshine) {
      var installedProducts = new List<InstalledProductInfo>();
      foreach (var hive in UninstallRegistryHives) {
        foreach (var root in UninstallRegistryRoots) {
          using (var uninstallRoot = hive.OpenSubKey(root)) {
            if (uninstallRoot != null) {
              CollectProductRegistrationsFromRoot(hive, uninstallRoot, root, installedProducts, includeSunshine);
            }
          }
        }
      }

      return installedProducts
        .GroupBy(BuildProductRegistrationIdentity, StringComparer.OrdinalIgnoreCase)
        .Select(MergeInstalledProductGroup)
        .ToList();
    }

    private static string BuildRegistryPath(RegistryKey hive, string rootPath, string subKeyName) {
      var hivePrefix = hive == Registry.LocalMachine ? "HKLM" : "HKCU";
      return hivePrefix + "\\" + rootPath + "\\" + subKeyName;
    }

    private static void CollectProductsFromRoot(RegistryKey hive, RegistryKey rootKey, string rootPath, List<InstalledProductInfo> output, bool includeSunshine) {
      foreach (var subKeyName in rootKey.GetSubKeyNames()) {
        if (string.IsNullOrWhiteSpace(subKeyName) || !LooksLikeProductCode(subKeyName)) {
          continue;
        }

        using (var productKey = rootKey.OpenSubKey(subKeyName)) {
          if (productKey == null) {
            continue;
          }

          var displayName = Convert.ToString(productKey.GetValue("DisplayName"));
          if (!IsWindowsInstallerProduct(productKey)) {
            continue;
          }

          var kind = GetInstalledProductKind(displayName);
          if (kind == InstalledProductKind.Unknown) {
            continue;
          }
          var uninstallString = Convert.ToString(productKey.GetValue("UninstallString")) ?? string.Empty;
          var quietUninstallString = Convert.ToString(productKey.GetValue("QuietUninstallString")) ?? string.Empty;
          if (IsBrowserWebAppRegistration(productKey, uninstallString, quietUninstallString)) {
            continue;
          }
          if (!includeSunshine && kind == InstalledProductKind.Sunshine) {
            continue;
          }

          var versionText = Convert.ToString(productKey.GetValue("DisplayVersion"));
          var parsedVersion = ParseVersion(versionText);

          output.Add(new InstalledProductInfo {
            ProductCode = subKeyName,
            DisplayName = displayName ?? string.Empty,
            Version = parsedVersion,
            Kind = kind,
            IsWindowsInstaller = true,
            IsPerUser = hive == Registry.CurrentUser,
            InstallLocation = ResolveInstallLocation(productKey, uninstallString, quietUninstallString),
            UninstallString = uninstallString,
            QuietUninstallString = quietUninstallString,
            RegistryPath = BuildRegistryPath(hive, rootPath, subKeyName)
          });
        }
      }
    }

    private static void CollectProductRegistrationsFromRoot(RegistryKey hive, RegistryKey rootKey, string rootPath, List<InstalledProductInfo> output, bool includeSunshine) {
      foreach (var subKeyName in rootKey.GetSubKeyNames()) {
        if (string.IsNullOrWhiteSpace(subKeyName)) {
          continue;
        }

        using (var productKey = rootKey.OpenSubKey(subKeyName)) {
          if (productKey == null) {
            continue;
          }

          var displayName = Convert.ToString(productKey.GetValue("DisplayName"));
          var kind = GetInstalledProductKind(displayName);
          if (kind == InstalledProductKind.Unknown) {
            continue;
          }
          var uninstallString = Convert.ToString(productKey.GetValue("UninstallString")) ?? string.Empty;
          var quietUninstallString = Convert.ToString(productKey.GetValue("QuietUninstallString")) ?? string.Empty;
          if (IsBrowserWebAppRegistration(productKey, uninstallString, quietUninstallString)) {
            continue;
          }
          if (!includeSunshine && kind == InstalledProductKind.Sunshine) {
            continue;
          }

          output.Add(new InstalledProductInfo {
            ProductCode = LooksLikeProductCode(subKeyName) ? subKeyName : string.Empty,
            DisplayName = displayName ?? string.Empty,
            Version = ParseVersion(Convert.ToString(productKey.GetValue("DisplayVersion"))),
            Kind = kind,
            IsWindowsInstaller = IsWindowsInstallerProduct(productKey) && LooksLikeProductCode(subKeyName),
            IsPerUser = hive == Registry.CurrentUser,
            InstallLocation = ResolveInstallLocation(productKey, uninstallString, quietUninstallString),
            UninstallString = uninstallString,
            QuietUninstallString = quietUninstallString,
            RegistryPath = BuildRegistryPath(hive, rootPath, subKeyName)
          });
        }
      }
    }

    private static bool IsWindowsInstallerProduct(RegistryKey productKey) {
      try {
        var value = productKey.GetValue("WindowsInstaller");
        if (value == null) {
          return false;
        }
        if (value is int) {
          return (int)value == 1;
        }
        if (value is string) {
          return string.Equals((string)value, "1", StringComparison.Ordinal);
        }
      } catch {
      }
      return false;
    }

    private static bool LooksLikeProductCode(string value) {
      return !string.IsNullOrWhiteSpace(value)
        && value.Length == 38
        && value.StartsWith("{", StringComparison.Ordinal)
        && value.EndsWith("}", StringComparison.Ordinal);
    }

    private static InstalledProductKind GetInstalledProductKind(string displayName) {
      if (string.IsNullOrWhiteSpace(displayName)) {
        return InstalledProductKind.Unknown;
      }

      var trimmedDisplayName = displayName.Trim();
      if (string.Equals(trimmedDisplayName, "Vibeshine", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Vibeshine;
      }
      if (string.Equals(trimmedDisplayName, "Vibepollo", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Vibepollo;
      }
      // Apollo is also a common prefix in unrelated software titles, so only
      // the exact streaming-host product name is considered a conflict.
      if (string.Equals(trimmedDisplayName, "Apollo", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Apollo;
      }
      if (string.Equals(trimmedDisplayName, "Sunshine", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Sunshine;
      }
      return InstalledProductKind.Unknown;
    }

    private static bool IsBrowserWebAppRegistration(
      RegistryKey productKey,
      string uninstallString,
      string quietUninstallString) {
      if (productKey == null) {
        return false;
      }

      var installLocation = Convert.ToString(productKey.GetValue("InstallLocation")) ?? string.Empty;
      var displayIcon = Convert.ToString(productKey.GetValue("DisplayIcon")) ?? string.Empty;
      if (ReferencesBrowserWebAppPath(installLocation) || ReferencesBrowserWebAppPath(displayIcon)) {
        return true;
      }

      return IsBrowserWebAppUninstallCommand(uninstallString)
        || IsBrowserWebAppUninstallCommand(quietUninstallString);
    }

    private static bool ReferencesBrowserWebAppPath(string value) {
      if (string.IsNullOrWhiteSpace(value)) {
        return false;
      }

      var normalized = Environment.ExpandEnvironmentVariables(value)
        .Trim()
        .Trim('"')
        .Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
      return normalized.IndexOf(@"\Web Applications\", StringComparison.OrdinalIgnoreCase) >= 0
        && normalized.IndexOf("_crx_", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static bool IsBrowserWebAppUninstallCommand(string commandLine) {
      string executablePath;
      string arguments;
      if (!TrySplitExecutableAndArguments(commandLine, out executablePath, out arguments)) {
        return false;
      }

      return IsBrowserExecutable(executablePath)
        && arguments.IndexOf("--uninstall-app-id", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static bool IsBrowserExecutable(string executablePath) {
      if (string.IsNullOrWhiteSpace(executablePath)) {
        return false;
      }

      var fileName = Path.GetFileName(executablePath.Trim().Trim('"'));
      return string.Equals(fileName, "chrome.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "msedge.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "brave.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "vivaldi.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "opera.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "opera_gx.exe", StringComparison.OrdinalIgnoreCase);
    }

    private static Version ParseVersion(string value) {
      if (string.IsNullOrWhiteSpace(value)) {
        return null;
      }

      Version encoded;
      if (TryEncodeSemanticVersion(value.Trim(), out encoded)) {
        return encoded;
      }

      Version parsed;
      if (Version.TryParse(value, out parsed)) {
        return parsed;
      }

      var numeric = new string(value.TakeWhile(ch => char.IsDigit(ch) || ch == '.').ToArray());
      if (Version.TryParse(numeric, out parsed)) {
        return parsed;
      }

      return null;
    }

    // Maps a three-part semantic version, optionally carrying a prerelease
    // suffix (the ARP DisplayVersion written by new installers, e.g.
    // "1.18.0-beta.2"), into the ordinal-encoded space used by new-scheme MSI
    // ProductVersions: third field = patch * 100 + ordinal, where alpha.N = N,
    // beta.N = 30 + N, rc.N = 60 + N, stable[.N] = 99 (mirrors
    // cmake/packaging/windows_wix.cmake). Keeps comparisons between ARP
    // registrations and MSI ProductVersions in one consistent ordering.
    // Four-part numeric strings are already ProductVersions and pass through
    // untouched via ParseVersion's Version.TryParse fallback.
    private static bool TryEncodeSemanticVersion(string value, out Version encoded) {
      encoded = null;
      if (string.IsNullOrWhiteSpace(value)) {
        return false;
      }

      var core = value;
      var prerelease = string.Empty;
      var plusIndex = core.IndexOf('+');
      if (plusIndex >= 0) {
        core = core.Substring(0, plusIndex);
      }
      var dashIndex = core.IndexOf('-');
      if (dashIndex >= 0) {
        prerelease = core.Substring(dashIndex + 1);
        core = core.Substring(0, dashIndex);
      }

      var parts = core.Split('.');
      if (parts.Length != 3) {
        return false;
      }

      int major;
      int minor;
      int patch;
      if (!int.TryParse(parts[0], out major)
          || !int.TryParse(parts[1], out minor)
          || !int.TryParse(parts[2], out patch)) {
        return false;
      }
      if (major < 0 || minor < 0 || patch < 0 || patch > 654) {
        return false;
      }

      encoded = new Version(major, minor, patch * 100 + GetPrereleaseOrdinal(prerelease), 0);
      return true;
    }

    private static int GetPrereleaseOrdinal(string prerelease) {
      if (string.IsNullOrWhiteSpace(prerelease)) {
        return 99;
      }

      var segments = prerelease.Split('.');
      var tag = segments[0].ToLowerInvariant();
      var number = 1;
      if (segments.Length > 1) {
        int parsedNumber;
        if (int.TryParse(segments[1], out parsedNumber)) {
          number = parsedNumber;
        }
      }
      if (number < 1) {
        number = 1;
      }
      if (number > 29) {
        number = 29;
      }

      if (string.Equals(tag, "alpha", StringComparison.Ordinal)) {
        return number;
      }
      if (string.Equals(tag, "beta", StringComparison.Ordinal)) {
        return 30 + number;
      }
      if (string.Equals(tag, "rc", StringComparison.Ordinal)) {
        return 60 + number;
      }
      if (string.Equals(tag, "stable", StringComparison.Ordinal)) {
        // Stable respins share the stable ordinal; sortable ProductCodes order
        // distinct MSI packages within that channel.
        return 99;
      }
      return 90;
    }

    private static void MergeProductMetadata(InstalledProductInfo target, InstalledProductInfo fallback) {
      if (target == null || fallback == null) {
        return;
      }

      if (string.IsNullOrWhiteSpace(target.InstallLocation)) {
        target.InstallLocation = fallback.InstallLocation ?? string.Empty;
      }
      if (string.IsNullOrWhiteSpace(target.UninstallString)) {
        target.UninstallString = fallback.UninstallString ?? string.Empty;
      }
      if (string.IsNullOrWhiteSpace(target.QuietUninstallString)) {
        target.QuietUninstallString = fallback.QuietUninstallString ?? string.Empty;
      }
      if (string.IsNullOrWhiteSpace(target.RegistryPath)) {
        target.RegistryPath = fallback.RegistryPath ?? string.Empty;
      }
    }

    private static InstalledProductInfo MergeInstalledProductGroup(IEnumerable<InstalledProductInfo> products) {
      var orderedProducts = products
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ThenByDescending(product => product.IsWindowsInstaller ? 1 : 0)
        .ThenByDescending(product => string.IsNullOrWhiteSpace(product.InstallLocation) ? 0 : 1)
        .ToList();
      var primary = orderedProducts.First();

      return new InstalledProductInfo {
        ProductCode = orderedProducts.Select(product => product.ProductCode).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty,
        DisplayName = orderedProducts.Select(product => product.DisplayName).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty,
        Version = orderedProducts.Select(product => product.Version).FirstOrDefault(value => value != null),
        Kind = primary.Kind,
        IsWindowsInstaller = orderedProducts.Any(product => product.IsWindowsInstaller),
        IsPerUser = primary.IsPerUser,
        InstallLocation = orderedProducts.Select(product => product.InstallLocation).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty,
        UninstallString = orderedProducts.Select(product => product.UninstallString).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty,
        QuietUninstallString = orderedProducts.Select(product => product.QuietUninstallString).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty,
        RegistryPath = orderedProducts.Select(product => product.RegistryPath).FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty
      };
    }

    private static string BuildProductRegistrationIdentity(InstalledProductInfo product) {
      if (product == null) {
        return string.Empty;
      }

      // Registry path is the uninstall-entry identity; separate hives can legitimately share a folder.
      if (!string.IsNullOrWhiteSpace(product.RegistryPath)) {
        return product.Kind + "|reg|" + product.RegistryPath;
      }
      if (product.IsWindowsInstaller && !string.IsNullOrWhiteSpace(product.ProductCode)) {
        return product.Kind + "|msi|" + product.ProductCode;
      }

      var command = string.IsNullOrWhiteSpace(product.QuietUninstallString)
        ? product.UninstallString
        : product.QuietUninstallString;
      if (!string.IsNullOrWhiteSpace(command)) {
        return product.Kind + "|cmd|" + NormalizeCommandIdentity(command);
      }

      if (!string.IsNullOrWhiteSpace(product.InstallLocation)) {
        return product.Kind + "|loc|" + product.InstallLocation;
      }

      return product.Kind + "|display|" + (product.DisplayName ?? string.Empty);
    }

    private static string ResolveInstallLocation(RegistryKey productKey, string uninstallString, string quietUninstallString) {
      var installLocation = NormalizeDirectoryPath(Convert.ToString(productKey.GetValue("InstallLocation")));
      if (!string.IsNullOrWhiteSpace(installLocation)) {
        return installLocation;
      }

      installLocation = TryResolveDirectoryFromFileReference(Convert.ToString(productKey.GetValue("DisplayIcon")));
      if (!string.IsNullOrWhiteSpace(installLocation)) {
        return installLocation;
      }

      installLocation = TryResolveDirectoryFromCommand(quietUninstallString);
      if (!string.IsNullOrWhiteSpace(installLocation)) {
        return installLocation;
      }

      return TryResolveDirectoryFromCommand(uninstallString);
    }

    private static string TryResolveDirectoryFromFileReference(string pathValue) {
      var trimmed = (pathValue ?? string.Empty).Trim();
      if (trimmed.Length == 0) {
        return string.Empty;
      }

      var commaIndex = trimmed.IndexOf(',');
      if (commaIndex > 0) {
        trimmed = trimmed.Substring(0, commaIndex);
      }

      trimmed = trimmed.Trim().Trim('"');
      if (trimmed.Length == 0) {
        return string.Empty;
      }

      var fullPath = NormalizePath(trimmed);
      if (string.IsNullOrWhiteSpace(fullPath)) {
        return string.Empty;
      }
      if (Directory.Exists(fullPath)) {
        return fullPath;
      }

      var parent = Path.GetDirectoryName(fullPath);
      return NormalizeDirectoryPath(parent);
    }

    private static string TryResolveDirectoryFromCommand(string commandLine) {
      if (string.IsNullOrWhiteSpace(commandLine)) {
        return string.Empty;
      }

      string executablePath;
      string arguments;
      if (!TrySplitExecutableAndArguments(commandLine, out executablePath, out arguments)) {
        return string.Empty;
      }

      var fullPath = NormalizePath(executablePath);
      if (string.IsNullOrWhiteSpace(fullPath) || IsMsiexecExecutable(fullPath)) {
        return string.Empty;
      }
      if (Directory.Exists(fullPath)) {
        return fullPath;
      }

      var parent = Path.GetDirectoryName(fullPath);
      return NormalizeDirectoryPath(parent);
    }

    private static string NormalizeDirectoryPath(string value) {
      var fullPath = NormalizePath(value);
      if (string.IsNullOrWhiteSpace(fullPath)) {
        return string.Empty;
      }
      if (Directory.Exists(fullPath)) {
        return fullPath;
      }

      var root = Path.GetPathRoot(fullPath);
      if (!string.IsNullOrWhiteSpace(root) && string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase)) {
        return root;
      }

      return fullPath;
    }

    private static string NormalizePath(string value) {
      var trimmed = Environment.ExpandEnvironmentVariables((value ?? string.Empty).Trim().Trim('"'));
      if (trimmed.Length == 0) {
        return string.Empty;
      }

      try {
        var fullPath = Path.GetFullPath(trimmed);
        var root = Path.GetPathRoot(fullPath);
        if (!string.IsNullOrWhiteSpace(root) && string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase)) {
          return root;
        }
        return fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
      } catch {
        return string.Empty;
      }
    }

    private static bool PathIsUnderDirectory(string path, string directory) {
      var normalizedPath = NormalizePath(path);
      var normalizedDirectory = NormalizePath(directory);
      if (string.IsNullOrWhiteSpace(normalizedPath) || string.IsNullOrWhiteSpace(normalizedDirectory)) {
        return false;
      }

      var directoryWithSeparator = normalizedDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
        + Path.DirectorySeparatorChar;
      return normalizedPath.Equals(normalizedDirectory, StringComparison.OrdinalIgnoreCase)
        || normalizedPath.StartsWith(directoryWithSeparator, StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeCommandIdentity(string commandLine) {
      return Environment.ExpandEnvironmentVariables(commandLine ?? string.Empty).Trim();
    }

    private static bool CanUninstallProduct(InstalledProductInfo product) {
      if (product == null) {
        return false;
      }

      if (product.IsWindowsInstaller && !string.IsNullOrWhiteSpace(product.ProductCode)) {
        return true;
      }

      return HasUsableUninstallCommand(product);
    }

    private static bool HasUsableUninstallCommand(InstalledProductInfo product) {
      var commandLine = BuildSilentUninstallCommand(product);
      if (string.IsNullOrWhiteSpace(commandLine)) {
        return false;
      }

      string executablePath;
      string arguments;
      if (!TrySplitExecutableAndArguments(commandLine, out executablePath, out arguments)) {
        return false;
      }

      return IsUsableExecutableReference(executablePath);
    }

    private static bool IsUsableExecutableReference(string executablePath) {
      if (string.IsNullOrWhiteSpace(executablePath)) {
        return false;
      }

      var trimmed = Environment.ExpandEnvironmentVariables(executablePath).Trim().Trim('"');
      if (trimmed.Length == 0) {
        return false;
      }

      var looksLikePath = trimmed.IndexOf('\\') >= 0 || trimmed.IndexOf('/') >= 0;
      if (!looksLikePath) {
        return true;
      }

      var fullPath = NormalizePath(trimmed);
      return !string.IsNullOrWhiteSpace(fullPath) && File.Exists(fullPath);
    }

    private const uint MsiErrorSuccess = 0;
    private const uint MsiErrorMoreData = 234;
    private const uint MsiErrorNoMoreItems = 259;
    private const uint MsiOpenPackageIgnoreMachineState = 1;
    private const int MsiNullInteger = int.MinValue;
    private const int MsiModifyUpdate = 2;
    private const uint GenericRead = 0x80000000;
    private const uint ReadControl = 0x00020000;
    private const uint FileReadAttributes = 0x00000080;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint OpenExisting = 3;
    private const uint FileFlagBackupSemantics = 0x02000000;
    private const uint FileFlagOpenReparsePoint = 0x00200000;
    private const uint FileAttributeReparsePoint = 0x00000400;
    private const uint FileNameNormalized = 0x0;
    private const uint VolumeNameDos = 0x0;
    private const uint WinTrustDataUiNone = 2;
    private const uint WinTrustDataRevokeWholeChain = 1;
    private const uint WinTrustDataChoiceFile = 1;
    private const uint WinTrustDataStateActionIgnore = 0;
    private const uint WinTrustDataSaferFlag = 0x00000100;
    private static readonly Guid WinTrustActionGenericVerifyV2 =
      new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
    // INSTALLSTATE_DEFAULT: Windows Installer reports the product as installed
    // and usable in the calling context.  Every other INSTALLSTATE value
    // (UNKNOWN, ADVERTISED, ABSENT, INVALIDARG, ...) means the ProductCode is
    // not a live per-machine installation.
    private const int MsiInstallStateDefault = 5;
    private static readonly IntPtr MsiDbOpenTransact = new IntPtr(1);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WinTrustFileInfo {
      public uint StructSize;
      public IntPtr FilePath;
      public IntPtr FileHandle;
      public IntPtr KnownSubject;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WinTrustData {
      public uint StructSize;
      public IntPtr PolicyCallbackData;
      public IntPtr SipClientData;
      public uint UiChoice;
      public uint RevocationChecks;
      public uint UnionChoice;
      public IntPtr FileInfo;
      public uint StateAction;
      public IntPtr StateData;
      public IntPtr UrlReference;
      public uint ProviderFlags;
      public uint UiContext;
      public IntPtr SignatureSettings;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation {
      public uint FileAttributes;
      public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
      public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
      public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
      public uint VolumeSerialNumber;
      public uint FileSizeHigh;
      public uint FileSizeLow;
      public uint NumberOfLinks;
      public uint FileIndexHigh;
      public uint FileIndexLow;
    }

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiOpenPackageEx(string szPackagePath, uint dwOptions, out IntPtr hProduct);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiGetProperty(IntPtr hInstall, string szName, StringBuilder szValueBuf, ref uint pchValueBuf);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern int MsiQueryProductState(string szProduct);

    [DllImport("msi.dll")]
    private static extern uint MsiCloseHandle(IntPtr hAny);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiGetProductInfo(string szProduct, string szAttribute, StringBuilder lpValueBuf, ref uint pcchValueBuf);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiOpenDatabase(string szDatabasePath, IntPtr szPersist, out IntPtr phDatabase);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiDatabaseOpenView(IntPtr hDatabase, string szQuery, out IntPtr phView);

    [DllImport("msi.dll")]
    private static extern uint MsiViewExecute(IntPtr hView, IntPtr hRecord);

    [DllImport("msi.dll")]
    private static extern uint MsiViewFetch(IntPtr hView, out IntPtr phRecord);

    [DllImport("msi.dll")]
    private static extern int MsiRecordGetInteger(IntPtr hRecord, uint iField);

    [DllImport("msi.dll")]
    private static extern uint MsiRecordDataSize(IntPtr hRecord, uint iField);

    [DllImport("msi.dll")]
    private static extern uint MsiRecordReadStream(
      IntPtr hRecord,
      uint iField,
      [Out] byte[] buffer,
      ref uint bufferSize);

    [DllImport("msi.dll")]
    private static extern uint MsiRecordSetInteger(IntPtr hRecord, uint iField, int iValue);

    [DllImport("msi.dll")]
    private static extern uint MsiViewModify(IntPtr hView, int eModifyMode, IntPtr hRecord);

    [DllImport("msi.dll")]
    private static extern uint MsiViewClose(IntPtr hView);

    [DllImport("msi.dll")]
    private static extern uint MsiDatabaseCommit(IntPtr hDatabase);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(
      string fileName,
      uint desiredAccess,
      uint shareMode,
      IntPtr securityAttributes,
      uint creationDisposition,
      uint flagsAndAttributes,
      IntPtr templateFile);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(
      SafeFileHandle file,
      StringBuilder filePath,
      uint filePathSize,
      uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(
      SafeFileHandle file,
      out ByHandleFileInformation information);

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern int WinVerifyTrust(
      IntPtr window,
      [In] ref Guid action,
      IntPtr trustData);

    // Add/Remove Programs keys outlive failed uninstalls, so registry evidence
    // alone cannot say whether a ProductCode is a real installation.  Ask
    // Windows Installer instead, and fail closed whenever it does not clearly
    // claim the product.
    private static bool IsInstalledProductCode(string productCode) {
      var normalized = NormalizeProductCode(productCode);
      if (!LooksLikeProductCode(normalized)) {
        return false;
      }

      try {
        if (MsiQueryProductState(normalized) == MsiInstallStateDefault) {
          return true;
        }
        // A ProductCode whose cached package is still registered is equally
        // real, and a cached package is exactly what the firewall-cleanup
        // recovery needs in order to act on the product at all.
        return !string.IsNullOrWhiteSpace(TryGetProductLocalPackagePath(normalized));
      } catch {
        return false;
      }
    }

    private static bool CanOpenMsiPackage(string msiPath) {
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return false;
      }

      IntPtr packageHandle;
      var openCode = MsiOpenPackageEx(msiPath, MsiOpenPackageIgnoreMachineState, out packageHandle);
      if (openCode != MsiErrorSuccess || packageHandle == IntPtr.Zero) {
        return false;
      }

      try {
        return true;
      } finally {
        MsiCloseHandle(packageHandle);
      }
    }

    public static InstallerResult RunInteractiveInstall(
      InstallerArguments arguments,
      string installDirectory,
      bool installVirtualDisplayDriver,
      bool saveInstallLogs,
      bool allowSelfElevation = true) {
      string payloadFailure;
      if (RejectUntrustedCliPayloadRequest(
            arguments,
            IsProcessElevated(),
            out payloadFailure)) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = payloadFailure
        };
      }
      try {
        installDirectory = NormalizeInstallPath(installDirectory);
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The install location is not trusted: " + ex.Message
        };
      }
      if (allowSelfElevation && !IsProcessElevated()) {
        return RunElevatedBootstrapperInstall(arguments, installDirectory, installVirtualDisplayDriver, saveInstallLogs);
      }

      SweepStaleInstallerRecoveryDirectories();

      string msiPath;
      try {
        msiPath = ResolveMsiPath(arguments == null ? null : arguments.MsiPathOverride);
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The installer could not resolve a valid MSI payload: " + ex.Message
        };
      }
      var recoveryDetails = new List<string>();
      StashedVibeshinePayload stashedPreviousPayload = null;
      var uninstallCompetingProductsResult = UninstallCompetingProducts(
        "install_remove_competing",
        true,
        false);
      var competingProductsRequireRestart = uninstallCompetingProductsResult.ExitCode == 3010;
      if (!uninstallCompetingProductsResult.Succeeded) {
        string recoveryDetail;
        if (TryRepairBustedMsiRegistration(
          uninstallCompetingProductsResult,
          new[] { InstalledProductKind.Vibepollo },
          "competing product pre-uninstall",
          out recoveryDetail)) {
          recoveryDetails.Add(recoveryDetail);
        } else {
          return new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = uninstallCompetingProductsResult.ExitCode,
            Message = BuildCompetingProductUninstallFailureMessage(uninstallCompetingProductsResult.Message),
            LogPath = uninstallCompetingProductsResult.LogPath
          };
        }
      }

      var restartRequired = competingProductsRequireRestart;

      StashedVibeshinePayload downgradeStash;
      var uninstallDowngradeSourceResult = TryPreUninstallDowngradeSourceVersion(
        msiPath,
        "install_remove_vibeshine_downgrade",
        true,
        false,
        out downgradeStash);
      AdoptStashedPayload(ref stashedPreviousPayload, downgradeStash);
      if (uninstallDowngradeSourceResult != null) {
        restartRequired |= uninstallDowngradeSourceResult.ExitCode == 3010;
        if (!uninstallDowngradeSourceResult.Succeeded) {
          string recoveryDetail;
          if (TryRepairBustedMsiRegistration(
            uninstallDowngradeSourceResult,
            new[] { InstalledProductKind.Vibeshine },
            "downgrade source pre-uninstall",
            out recoveryDetail)) {
            recoveryDetails.Add(recoveryDetail);
          } else {
            return ApplyStashedPayloadRecovery(new InstallerResult {
              Operation = InstallerOperation.Install,
              ExitCode = uninstallDowngradeSourceResult.ExitCode,
              Message = BuildDowngradeSourcePreUninstallFailureMessage(uninstallDowngradeSourceResult.Message),
              LogPath = uninstallDowngradeSourceResult.LogPath
            }, stashedPreviousPayload, "install_restore_previous");
          }
        }
      }

      StashedVibeshinePayload upgradeSourceStash;
      var uninstallUpgradeSourceResult = TryPreUninstallProblematicUpgradeSourceVersion(
        "install_remove_vibepollo_1148",
        true,
        false,
        out upgradeSourceStash);
      AdoptStashedPayload(ref stashedPreviousPayload, upgradeSourceStash);
      if (uninstallUpgradeSourceResult != null) {
        restartRequired |= uninstallUpgradeSourceResult.ExitCode == 3010;
        if (!uninstallUpgradeSourceResult.Succeeded) {
          string recoveryDetail;
          if (TryRepairBustedMsiRegistration(
            uninstallUpgradeSourceResult,
            new[] { InstalledProductKind.Vibepollo },
            "upgrade source pre-uninstall",
            out recoveryDetail)) {
            recoveryDetails.Add(recoveryDetail);
          } else {
            return ApplyStashedPayloadRecovery(new InstallerResult {
              Operation = InstallerOperation.Install,
              ExitCode = uninstallUpgradeSourceResult.ExitCode,
              Message = BuildUpgradeSourcePreUninstallFailureMessage(uninstallUpgradeSourceResult.Message),
              LogPath = uninstallUpgradeSourceResult.LogPath
            }, stashedPreviousPayload, "install_restore_previous");
          }
        }
      }

      var migrationCleanupResult = RunPreinstallMigrationCleanup("preinstall", true, false);
      if (!migrationCleanupResult.Succeeded) {
        string recoveryDetail;
        if (TryRepairBustedMsiRegistration(
          migrationCleanupResult,
          new[] { InstalledProductKind.Vibeshine },
          "preinstall migration cleanup",
          out recoveryDetail)) {
          recoveryDetails.Add(recoveryDetail);
          migrationCleanupResult = RunPreinstallMigrationCleanup(
            "preinstall_registration_recovery",
            true,
            false);
        }
      }
      if (migrationCleanupResult.ExitCode != 0) {
        migrationCleanupResult.Operation = InstallerOperation.Install;
        migrationCleanupResult.InstallDeferredForRestart = migrationCleanupResult.ExitCode == 3010;
        if (migrationCleanupResult.InstallDeferredForRestart) {
          migrationCleanupResult.ExitCode = InstallDeferredRebootRequiredExitCode;
        }
        migrationCleanupResult.Message = migrationCleanupResult.InstallDeferredForRestart
          ? "Installation is deferred. " + migrationCleanupResult.Message
          : "Install could not continue. " + migrationCleanupResult.Message;
        AppendRecoveryDetails(migrationCleanupResult, recoveryDetails);
        return ApplyStashedPayloadRecovery(
          migrationCleanupResult,
          stashedPreviousPayload,
          "install_restore_previous");
      }
      var installResult = RunInstallAttempt(
        msiPath,
        installDirectory,
        installVirtualDisplayDriver,
        saveInstallLogs,
        restartRequired,
        "install");

      if (ShouldRetryInstallWithFreshPayload(arguments, msiPath, installResult)) {
        TryDeleteFile(msiPath);
        var initialLogPath = installResult.LogPath;
        string refreshedMsiPath;
        try {
          refreshedMsiPath = ResolveMsiPath(null, true);
        } catch (Exception ex) {
          var refreshFailure = new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = 1603,
            Message = "The installer could not extract a fresh MSI payload: " + ex.Message,
            LogPath = initialLogPath
          };
          AppendRecoveryDetails(refreshFailure, recoveryDetails);
          return ApplyStashedPayloadRecovery(
            refreshFailure,
            stashedPreviousPayload,
            "install_restore_previous");
        }
        installResult = RunInstallAttempt(
          refreshedMsiPath,
          installDirectory,
          installVirtualDisplayDriver,
          saveInstallLogs,
          restartRequired,
          "install_recovery");
        msiPath = refreshedMsiPath;
        if (!installResult.Succeeded && !string.IsNullOrWhiteSpace(initialLogPath)) {
          installResult.Message += " Initial attempt log: " + initialLogPath;
        }
      }

      bool firewallRecoveryRequiresRestart;
      string firewallRecoveryDetail;
      StashedVibeshinePayload firewallRecoveryStash;
      if (TryRecoverMsiFirewallCleanupFailure(
        installResult,
        MsiRegistrationRecoveryKinds,
        "install upgrade",
        true,
        false,
        out firewallRecoveryRequiresRestart,
        out firewallRecoveryDetail,
        out firewallRecoveryStash)) {
        AdoptStashedPayload(ref stashedPreviousPayload, firewallRecoveryStash);
        recoveryDetails.Add(firewallRecoveryDetail);
        restartRequired |= firewallRecoveryRequiresRestart;
        var initialLogPath = installResult.LogPath;
        installResult = RunInstallAttempt(
          msiPath,
          installDirectory,
          installVirtualDisplayDriver,
          saveInstallLogs,
          restartRequired,
          "install_firewall_cleanup_recovery");
        if (!installResult.Succeeded && !string.IsNullOrWhiteSpace(initialLogPath)) {
          installResult.Message += " Initial attempt log: " + initialLogPath;
        }
      }

      if (ShouldRepairBustedMsiRegistration(installResult, MsiRegistrationRecoveryKinds)) {
        string recoveryDetail;
        if (TryRepairBustedMsiRegistration(
          installResult,
          MsiRegistrationRecoveryKinds,
          "install upgrade",
          out recoveryDetail)) {
          recoveryDetails.Add(recoveryDetail);
          var initialLogPath = installResult.LogPath;
          installResult = RunInstallAttempt(
            msiPath,
            installDirectory,
            installVirtualDisplayDriver,
            saveInstallLogs,
            restartRequired,
            "install_registration_recovery");
          if (!installResult.Succeeded && !string.IsNullOrWhiteSpace(initialLogPath)) {
            installResult.Message += " Initial attempt log: " + initialLogPath;
          }
        }
      }

      AppendRecoveryDetails(installResult, recoveryDetails);
      return ApplyStashedPayloadRecovery(installResult, stashedPreviousPayload, "install_restore_previous");
    }

    private static InstallerResult RunInstallAttempt(
      string msiPath,
      string installDirectory,
      bool installVirtualDisplayDriver,
      bool saveInstallLogs,
      bool competingProductsRequireRestart,
      string logPhase) {
      var logPath = BuildLogPath(logPhase);
      var args = new List<string> {
        "/i",
        msiPath,
        "/qn",
        "/norestart",
        "/l*v",
        logPath,
        CreatePropertyArgument("INSTALL_ROOT", installDirectory),
        "INSTALL_VIRTUAL_DISPLAY_DRIVER=" + (installVirtualDisplayDriver ? "1" : "0"),
        "SKIP_REMOVE_CONFLICTING_PRODUCTS=1",
        "REBOOT=ReallySuppress",
        "SUPPRESSMSGBOXES=1"
      };
      TryAppendSameProductReinstallProperties(args, msiPath);

      AppendInstallerLogMessage(logPath, "Quiescing related services and helper processes before MSI install attempt.");
      TryStopRelatedServicesAndProcesses(logPath);

      var registrationRecoveryProduct =
        TryGetUnambiguousMsiRegistrationRecoveryProduct(InstalledProductKind.Vibepollo);
      var exitCode = RunMsiexec(args, true, false);
      exitCode = RetryInstallWithSameProductReinstallIfNeeded(exitCode, args, msiPath, true, false);
      if (exitCode == 0 && competingProductsRequireRestart) {
        exitCode = 3010;
      }
      if (exitCode != 0 && exitCode != 3010) {
        TryRecoverServiceStateAfterFailedInstall();
      }
      if (exitCode == 0 && InstallLogIndicatesDriverRebootRequired(logPath)) {
        exitCode = 3010;
      }
      string validationFailure;
      if ((exitCode == 0 || exitCode == 3010) && !ValidatePayloadRegisteredAfterInstall(msiPath, logPath, out validationFailure)) {
        AppendInstallerLogMessage(logPath, validationFailure);
        exitCode = 1603;
      }

      var componentFailures = CollectInstallComponentFailures(logPath, installVirtualDisplayDriver);
      var savedLogPath = string.Empty;
      var saveLogsWarning = string.Empty;
      var saveLogsDetail = string.Empty;
      if (saveInstallLogs) {
        try {
          savedLogPath = PersistInstallLog(logPath, installDirectory, logPhase);
        } catch (Exception ex) {
          saveLogsWarning = ex.Message;
        }
      }

      var resultMessage = BuildResultMessage("Install", exitCode, logPath);
      if (saveInstallLogs) {
        if (!string.IsNullOrWhiteSpace(savedLogPath)) {
          resultMessage += " Saved log copy: " + savedLogPath;
          saveLogsDetail = "Saved installer log: " + savedLogPath;
        } else if (!string.IsNullOrWhiteSpace(saveLogsWarning)) {
          resultMessage += " Could not save install log copy: " + saveLogsWarning;
          saveLogsDetail = "Could not save installer log copy: " + saveLogsWarning;
        } else {
          resultMessage += " Could not save install log copy.";
          saveLogsDetail = "Could not save installer log copy.";
        }
      }

      if (componentFailures.Count > 0) {
        var componentSummary = "Component warnings: " + string.Join(" ", componentFailures);
        resultMessage += " " + componentSummary;
        if (string.IsNullOrWhiteSpace(saveLogsDetail)) {
          saveLogsDetail = componentSummary;
        } else {
          saveLogsDetail += "\n" + componentSummary;
        }
      }

      var result = new InstallerResult {
        Operation = InstallerOperation.Install,
        ExitCode = exitCode,
        Message = resultMessage,
        UserDetail = saveLogsDetail,
        LogPath = logPath,
        ComponentFailures = componentFailures
      };
      AttachMsiRegistrationRecoveryProduct(result, registrationRecoveryProduct);
      return result;
    }

    private static InstallerResult ManualLegacyRemovalRequired(string product) {
      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = 1603,
        Message = (product ?? "A legacy product")
          + " must be removed manually before continuing. Vibepollo will not execute an older or registry-sourced uninstaller with elevated privileges before its native MSI trust gate."
      };
    }

    private static InstallerResult UninstallLegacySunshineRegistration() {
      return ManualLegacyRemovalRequired("legacy Sunshine");
    }

    private static InstallerResult UninstallLegacyApolloRegistration() {
      return ManualLegacyRemovalRequired("legacy Apollo");
    }


    private static bool ValidatePayloadRegisteredAfterInstall(string msiPath, string logPath, out string failureMessage) {
      failureMessage = string.Empty;

      var payloadMsiInfo = TryGetPayloadMsiInfo(msiPath);
      var payloadProductCode = NormalizeProductCode(payloadMsiInfo == null ? null : payloadMsiInfo.ProductCode);
      if (!LooksLikeProductCode(payloadProductCode)) {
        return true;
      }

      var installedProduct = GetInstalledProducts(true)
        .FirstOrDefault(product => string.Equals(
          NormalizeProductCode(product.ProductCode),
          payloadProductCode,
          StringComparison.OrdinalIgnoreCase));
      if (installedProduct != null) {
        return true;
      }

      failureMessage = "MSI reported success, but the expected product registration was not found after install. Expected ProductCode: "
        + payloadProductCode + ".";
      if (!string.IsNullOrWhiteSpace(logPath)) {
        failureMessage += " Log: " + logPath;
      }
      return false;
    }

    private static bool ShouldRetryInstallWithFreshPayload(
      InstallerArguments arguments,
      string attemptedMsiPath,
      InstallerResult installResult) {
      if (installResult == null || installResult.Succeeded) {
        return false;
      }
      if (arguments != null && !string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        return false;
      }
      if (string.IsNullOrWhiteSpace(attemptedMsiPath) || !IsInstallerPayloadPath(attemptedMsiPath)) {
        return false;
      }

      return LogShowsMsiAccessFailure(installResult.LogPath, attemptedMsiPath)
        || !WaitForMsiPackageAvailability(attemptedMsiPath, 1, 0);
    }

    private static bool IsInstallerPayloadPath(string msiPath) {
      if (string.IsNullOrWhiteSpace(msiPath)) {
        return false;
      }

      try {
        foreach (var root in GetEmbeddedMsiExtractRoots()) {
          if (PathIsUnderDirectory(msiPath, root)) {
            return true;
          }
        }
      } catch {
      }

      return false;
    }

    private static bool LogShowsMsiAccessFailure(string logPath, string msiPath) {
      if (string.IsNullOrWhiteSpace(logPath) || !File.Exists(logPath)) {
        return false;
      }

      var expectedPath = msiPath ?? string.Empty;
      var expectedFileName = Path.GetFileName(expectedPath);

      try {
        foreach (var line in File.ReadLines(logPath)) {
          if (string.IsNullOrWhiteSpace(line)) {
            continue;
          }

          var hasAccessFailureText =
            line.IndexOf("Failed to access database:", StringComparison.OrdinalIgnoreCase) >= 0
            || line.IndexOf("The installation package could not be opened", StringComparison.OrdinalIgnoreCase) >= 0;
          if (!hasAccessFailureText) {
            continue;
          }

          if (expectedPath.Length == 0) {
            return true;
          }
          if (line.IndexOf(expectedPath, StringComparison.OrdinalIgnoreCase) >= 0) {
            return true;
          }
          if (!string.IsNullOrWhiteSpace(expectedFileName)
              && line.IndexOf(expectedFileName, StringComparison.OrdinalIgnoreCase) >= 0) {
            return true;
          }
        }
      } catch {
      }

      return false;
    }

    private static bool ShouldRepairBustedMsiRegistration(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds) {
      if (failureResult == null || failureResult.Succeeded) {
        return false;
      }
      return IsRecoverableMsiCacheFailure(failureResult.ExitCode, failureResult.LogPath);
    }

    private static bool IsRecoverableMsiCacheFailure(int exitCode, string logPath) {
      if (exitCode == 1612 || exitCode == 1610) {
        return true;
      }
      if (exitCode != 1603) {
        return false;
      }
      return LogShowsMsiCacheOrSourceFailure(logPath);
    }

    private static bool IsRecoverableMsiFirewallCleanupFailure(int exitCode, string logPath) {
      return exitCode == 1603 && LogContainsMarker(logPath, MsiFirewallExceptionUninstallFailureLogMarker);
    }

    private const string InstallerRecoveryDirectoryPrefix = "VibepolloInstallerRecovery_";

    // A retained recovery stash is only useful while the user is still acting
    // on the failure message that named it, so keep it for an hour and then let
    // the next elevated install/uninstall reclaim the space.  Without this the
    // per-attempt directories (each holding a full cached MSI) accumulated
    // forever under %WINDIR%\Temp.
    private static readonly TimeSpan StaleInstallerRecoveryDirectoryAge = TimeSpan.FromHours(1);

    // Recovery directories created by this process are never swept, no matter
    // how long the run takes.
    private static readonly HashSet<string> ActiveInstallerRecoveryDirectories =
      new HashSet<string>(StringComparer.OrdinalIgnoreCase);

    private static string TryGetFullPathOrNull(string path) {
      try {
        if (string.IsNullOrWhiteSpace(path)) {
          return null;
        }
        return Path.GetFullPath(path)
          .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
      } catch {
        return null;
      }
    }

    private static void RegisterActiveInstallerRecoveryDirectory(string recoveryDirectory) {
      var fullPath = TryGetFullPathOrNull(recoveryDirectory);
      if (string.IsNullOrWhiteSpace(fullPath)) {
        return;
      }
      lock (ActiveInstallerRecoveryDirectories) {
        ActiveInstallerRecoveryDirectories.Add(fullPath);
      }
    }

    private static bool IsActiveInstallerRecoveryDirectory(string recoveryDirectory) {
      var fullPath = TryGetFullPathOrNull(recoveryDirectory);
      if (string.IsNullOrWhiteSpace(fullPath)) {
        // Unresolvable paths are treated as in use so the sweep never guesses.
        return true;
      }
      lock (ActiveInstallerRecoveryDirectories) {
        return ActiveInstallerRecoveryDirectories.Contains(fullPath);
      }
    }

    // Swept once on entry to an elevated install/uninstall flow, before this
    // run creates any recovery directory of its own.  Deletion is delegated to
    // TryDeleteInstallerRecoveryDirectory so the parent-directory and name
    // guards there remain the single place that authorizes a removal.
    private static void SweepStaleInstallerRecoveryDirectories() {
      try {
        if (!IsProcessElevated()) {
          return;
        }
        var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (string.IsNullOrWhiteSpace(windowsDirectory)) {
          return;
        }
        var recoveryRoot = Path.Combine(windowsDirectory, "Temp");
        if (!Directory.Exists(recoveryRoot)) {
          return;
        }

        var cutoffUtc = DateTime.UtcNow - StaleInstallerRecoveryDirectoryAge;
        foreach (var candidate in Directory.GetDirectories(recoveryRoot, InstallerRecoveryDirectoryPrefix + "*")) {
          try {
            if (IsActiveInstallerRecoveryDirectory(candidate)) {
              continue;
            }
            var candidateInfo = new DirectoryInfo(candidate);
            if ((candidateInfo.Attributes & FileAttributes.ReparsePoint) != 0) {
              continue;
            }
            // A concurrently running bootstrapper's freshly created stash is
            // still young, so the age check also protects other processes.
            if (candidateInfo.CreationTimeUtc > cutoffUtc || candidateInfo.LastWriteTimeUtc > cutoffUtc) {
              continue;
            }
            TryDeleteInstallerRecoveryDirectory(candidateInfo.FullName);
          } catch {
          }
        }
      } catch {
      }
    }

    private static DirectorySecurity BuildInstallerRecoveryDirectorySecurity() {
      var security = new DirectorySecurity();
      security.SetAccessRuleProtection(true, false);
      var fullControl = FileSystemRights.FullControl;
      var inheritance = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
      var administratorsSid = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
      security.SetOwner(administratorsSid);
      security.AddAccessRule(new FileSystemAccessRule(
        new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
        fullControl,
        inheritance,
        PropagationFlags.None,
        AccessControlType.Allow));
      security.AddAccessRule(new FileSystemAccessRule(
        administratorsSid,
        fullControl,
        inheritance,
        PropagationFlags.None,
        AccessControlType.Allow));
      return security;
    }

    private static bool TryCreateSecureInstallerRecoveryDirectory(
      out string recoveryDirectory,
      out string errorMessage) {
      recoveryDirectory = string.Empty;
      errorMessage = string.Empty;
      try {
        var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (string.IsNullOrWhiteSpace(windowsDirectory)) {
          errorMessage = "The Windows directory could not be resolved.";
          return false;
        }

        recoveryDirectory = Path.Combine(
          windowsDirectory,
          "Temp",
          InstallerRecoveryDirectoryPrefix + Guid.NewGuid().ToString("N"));
        RegisterActiveInstallerRecoveryDirectory(recoveryDirectory);
        var recoveryInfo = new DirectoryInfo(recoveryDirectory);
        recoveryInfo.Create(BuildInstallerRecoveryDirectorySecurity());
        recoveryInfo.SetAccessControl(BuildInstallerRecoveryDirectorySecurity());
        recoveryInfo.Refresh();
        if ((recoveryInfo.Attributes & FileAttributes.ReparsePoint) != 0) {
          errorMessage = "The installer recovery directory is a reparse point.";
          return false;
        }
        return true;
      } catch (Exception ex) {
        errorMessage = ex.Message;
        return false;
      }
    }

    private static void TryDeleteInstallerRecoveryDirectory(string recoveryDirectory) {
      try {
        if (string.IsNullOrWhiteSpace(recoveryDirectory) || !Directory.Exists(recoveryDirectory)) {
          return;
        }
        var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (string.IsNullOrWhiteSpace(windowsDirectory)) {
          return;
        }
        var recoveryRoot = Path.GetFullPath(Path.Combine(windowsDirectory, "Temp"))
          .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var fullRecoveryDirectory = Path.GetFullPath(recoveryDirectory)
          .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var recoveryName = Path.GetFileName(fullRecoveryDirectory);
        if (!string.Equals(
              Path.GetDirectoryName(fullRecoveryDirectory),
              recoveryRoot,
              StringComparison.OrdinalIgnoreCase)
            || string.IsNullOrWhiteSpace(recoveryName)
            || !recoveryName.StartsWith(InstallerRecoveryDirectoryPrefix, StringComparison.Ordinal)) {
          return;
        }
        var recoveryInfo = new DirectoryInfo(fullRecoveryDirectory);
        if ((recoveryInfo.Attributes & FileAttributes.ReparsePoint) != 0) {
          return;
        }
        recoveryInfo.Delete(true);
      } catch {
      }
    }

    private static bool TryCreateFirewallTolerantMsiCopy(
      string productCode,
      out string recoveryDirectory,
      out string originalMsiPath,
      out string workingMsiPath,
      out string errorMessage) {
      recoveryDirectory = string.Empty;
      originalMsiPath = string.Empty;
      workingMsiPath = string.Empty;
      errorMessage = string.Empty;

      var normalizedProductCode = NormalizeProductCode(productCode);
      if (!LooksLikeProductCode(normalizedProductCode)) {
        errorMessage = "The failed MSI product code was not valid.";
        return false;
      }

      var cachedMsiPath = TryGetProductLocalPackagePath(normalizedProductCode);
      if (string.IsNullOrWhiteSpace(cachedMsiPath) || !File.Exists(cachedMsiPath)) {
        errorMessage = "Windows Installer did not expose an accessible cached package for " + normalizedProductCode + ".";
        return false;
      }

      IntPtr modifiedDatabase = IntPtr.Zero;
      IntPtr view = IntPtr.Zero;
      try {
        if (!TryCreateSecureInstallerRecoveryDirectory(out recoveryDirectory, out errorMessage)) {
          return false;
        }

        originalMsiPath = Path.Combine(recoveryDirectory, "original_cached.msi");
        workingMsiPath = Path.Combine(recoveryDirectory, "firewall_cleanup.msi");
        File.Copy(cachedMsiPath, originalMsiPath, false);
        File.Copy(originalMsiPath, workingMsiPath, false);

        var copiedMsiInfo = TryGetPayloadMsiInfo(originalMsiPath);
        if (copiedMsiInfo == null
            || !string.Equals(
              NormalizeProductCode(copiedMsiInfo.ProductCode),
              normalizedProductCode,
              StringComparison.OrdinalIgnoreCase)) {
          errorMessage = "The cached MSI ProductCode did not match the exact failed product.";
          return false;
        }
        copiedMsiInfo = TryGetPayloadMsiInfo(workingMsiPath);
        if (copiedMsiInfo == null
            || !string.Equals(
              NormalizeProductCode(copiedMsiInfo.ProductCode),
              normalizedProductCode,
              StringComparison.OrdinalIgnoreCase)) {
          errorMessage = "The working MSI copy did not match the exact failed product.";
          return false;
        }

        var openCode = MsiOpenDatabase(workingMsiPath, MsiDbOpenTransact, out modifiedDatabase);
        if (openCode != MsiErrorSuccess || modifiedDatabase == IntPtr.Zero) {
          errorMessage = "Could not open the copied MSI database for a transactional edit (error " + openCode + ").";
          return false;
        }

        var viewCode = MsiDatabaseOpenView(
          modifiedDatabase,
          "SELECT `WixFirewallException`, `Attributes` FROM `WixFirewallException`",
          out view);
        if (viewCode != MsiErrorSuccess || view == IntPtr.Zero) {
          errorMessage = "Could not query the WixFirewallException table (error " + viewCode + ").";
          return false;
        }

        var executeCode = MsiViewExecute(view, IntPtr.Zero);
        if (executeCode != MsiErrorSuccess) {
          errorMessage = "Could not read the WixFirewallException table (error " + executeCode + ").";
          return false;
        }

        var updatedRows = 0;
        uint fetchCode;
        IntPtr record;
        while ((fetchCode = MsiViewFetch(view, out record)) == MsiErrorSuccess) {
          try {
            var attributes = MsiRecordGetInteger(record, 2);
            if (attributes == MsiNullInteger) {
              attributes = 0;
            }
            if ((attributes & 0x1) != 0) {
              continue;
            }
            var setCode = MsiRecordSetInteger(record, 2, attributes | 0x1);
            if (setCode != MsiErrorSuccess) {
              errorMessage = "Could not update a firewall exception row (error " + setCode + ").";
              return false;
            }
            var modifyCode = MsiViewModify(view, MsiModifyUpdate, record);
            if (modifyCode != MsiErrorSuccess) {
              errorMessage = "Could not persist a firewall exception row (error " + modifyCode + ").";
              return false;
            }
            updatedRows++;
          } finally {
            if (record != IntPtr.Zero) {
              MsiCloseHandle(record);
            }
          }
        }
        if (fetchCode != MsiErrorNoMoreItems) {
          errorMessage = "Could not finish reading the WixFirewallException table (error " + fetchCode + ").";
          return false;
        }
        if (updatedRows == 0) {
          errorMessage = "The cached MSI had no firewall exception rows requiring the IgnoreFailure flag.";
          return false;
        }

        MsiViewClose(view);
        MsiCloseHandle(view);
        view = IntPtr.Zero;

        var commitCode = MsiDatabaseCommit(modifiedDatabase);
        if (commitCode != MsiErrorSuccess) {
          errorMessage = "Could not commit the temporary MSI edit (error " + commitCode + ").";
          return false;
        }

        MsiCloseHandle(modifiedDatabase);
        modifiedDatabase = IntPtr.Zero;

        copiedMsiInfo = TryGetPayloadMsiInfo(workingMsiPath);
        if (copiedMsiInfo == null
            || !string.Equals(
              NormalizeProductCode(copiedMsiInfo.ProductCode),
              normalizedProductCode,
              StringComparison.OrdinalIgnoreCase)) {
          errorMessage = "The edited MSI copy no longer matched the exact failed ProductCode.";
          return false;
        }
        return true;
      } catch (Exception ex) {
        errorMessage = ex.Message;
        return false;
      } finally {
        if (view != IntPtr.Zero) {
          MsiViewClose(view);
          MsiCloseHandle(view);
        }
        if (modifiedDatabase != IntPtr.Zero) {
          MsiCloseHandle(modifiedDatabase);
        }
      }
    }

    private static bool TryRunFirewallTolerantUninstall(
      InstalledProductInfo product,
      IReadOnlyList<string> originalArguments,
      string originalLogPath,
      string retryLogPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      bool preserveOriginalPayload,
      out int retryExitCode,
      out string retryLogPath,
      out string recoveryDetail,
      out StashedVibeshinePayload stashedPayload) {
      retryExitCode = 1603;
      retryLogPath = originalLogPath ?? string.Empty;
      recoveryDetail = string.Empty;
      stashedPayload = null;
      if (!IsProcessElevated()
          || product == null
          || !IsRecoveryKindAllowed(product.Kind, MsiRegistrationRecoveryKinds)
          || !LooksLikeProductCode(product.ProductCode)
          || originalArguments == null) {
        return false;
      }

      string recoveryDirectory;
      string originalMsiPath;
      string workingMsiPath;
      string copyError;
      if (!TryCreateFirewallTolerantMsiCopy(
        product.ProductCode,
        out recoveryDirectory,
        out originalMsiPath,
        out workingMsiPath,
        out copyError)) {
        AppendInstallerLogMessage(
          originalLogPath,
          "Exact-product firewall cleanup recovery was not available: " + copyError);
        if (!string.IsNullOrWhiteSpace(recoveryDirectory)) {
          TryDeleteInstallerRecoveryDirectory(recoveryDirectory);
        }
        return false;
      }

      try {
        retryLogPath = BuildLogPath(retryLogPhase);
        var retryArguments = new List<string>(originalArguments);
        if (!ReplaceMsiUninstallTarget(
          retryArguments,
          product.ProductCode,
          workingMsiPath)) {
          AppendInstallerLogMessage(
            originalLogPath,
            "Exact-product firewall cleanup recovery could not replace the validated uninstall target with the edited MSI copy.");
          return false;
        }
        if (!ReplaceArgumentValue(retryArguments, originalLogPath, retryLogPath)) {
          retryArguments.Add("/l*v");
          retryArguments.Add(retryLogPath);
        }

        AppendInstallerLogMessage(
          originalLogPath,
          "Retrying normal MSI uninstall for exact product " + NormalizeProductCode(product.ProductCode)
          + " from a secured, ProductCode-verified copy of its cached package that marks only firewall-rule cleanup failures non-fatal.");
        retryExitCode = RunMsiexec(retryArguments, hiddenWindow, requestElevationIfNeeded);
        recoveryDetail = "Retried the exact cached MSI copy with firewall cleanup marked non-fatal. Retry log: "
          + retryLogPath;
        AppendInstallerLogMessage(retryLogPath, recoveryDetail);
        if (preserveOriginalPayload
            && product.Kind == InstalledProductKind.Vibepollo
            && (retryExitCode == 0 || retryExitCode == 3010 || retryExitCode == 1605)) {
          stashedPayload = new StashedVibeshinePayload {
            MsiPath = originalMsiPath,
            ProductCode = NormalizeProductCode(product.ProductCode),
            InstallLocation = product.InstallLocation ?? string.Empty,
            RecoveryDirectory = recoveryDirectory
          };
        }
        return true;
      } finally {
        if (stashedPayload == null) {
          TryDeleteInstallerRecoveryDirectory(recoveryDirectory);
        } else {
          TryDeleteFile(workingMsiPath);
        }
      }
    }

    private static bool TryRecoverMsiFirewallCleanupFailure(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds,
      string context,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      out bool restartRequired,
      out string recoveryDetail,
      out StashedVibeshinePayload stashedPayload) {
      restartRequired = false;
      recoveryDetail = string.Empty;
      stashedPayload = null;
      if (failureResult == null
          || !IsRecoverableMsiFirewallCleanupFailure(failureResult.ExitCode, failureResult.LogPath)
          || !HasConcreteMsiRegistrationRecoveryProduct(failureResult, allowedKinds)) {
        return false;
      }

      var installedProduct = GetInstalledProducts(false).FirstOrDefault(candidate =>
        candidate.Kind == failureResult.ProductKind
        && string.Equals(
          NormalizeProductCode(candidate.ProductCode),
          NormalizeProductCode(failureResult.ProductCode),
          StringComparison.OrdinalIgnoreCase));
      var product = new InstalledProductInfo {
        ProductCode = NormalizeProductCode(failureResult.ProductCode),
        DisplayName = failureResult.ProductDisplayName ?? string.Empty,
        Kind = failureResult.ProductKind,
        IsWindowsInstaller = true,
        InstallLocation = installedProduct == null ? string.Empty : installedProduct.InstallLocation
      };
      var retryLogPhase = (context ?? "msi").Replace(' ', '_') + "_firewall_cleanup_recovery";
      var args = new List<string> {
        "/x",
        product.ProductCode,
        "/qn",
        "/norestart",
        "/l*v",
        failureResult.LogPath,
        "REBOOT=ReallySuppress",
        "SUPPRESSMSGBOXES=1"
      };

      int retryExitCode;
      string retryLogPath;
      if (!TryRunFirewallTolerantUninstall(
        product,
        args,
        failureResult.LogPath,
        retryLogPhase,
        hiddenWindow,
        requestElevationIfNeeded,
        true,
        out retryExitCode,
        out retryLogPath,
        out recoveryDetail,
        out stashedPayload)) {
        return false;
      }
      if (retryExitCode != 0 && retryExitCode != 3010 && retryExitCode != 1605) {
        AppendInstallerLogMessage(
          retryLogPath,
          "Exact-product firewall cleanup recovery failed with exit code " + retryExitCode + ".");
        return false;
      }
      restartRequired = retryExitCode == 3010;

      recoveryDetail = "Recovered from the previous MSI firewall cleanup failure by normally uninstalling "
        + BuildProductDisplayName(product)
        + " from a secured, exact-product copy of its cached MSI. Retry log: "
        + retryLogPath;
      AppendInstallerLogMessage(retryLogPath, recoveryDetail);
      return true;
    }

    private static bool LogShowsMsiCacheOrSourceFailure(string logPath) {
      if (string.IsNullOrWhiteSpace(logPath) || !File.Exists(logPath)) {
        return false;
      }

      try {
        foreach (var line in File.ReadLines(logPath)) {
          if (string.IsNullOrWhiteSpace(line)) {
            continue;
          }
          foreach (var marker in MsiCacheFailureLogMarkers) {
            if (line.IndexOf(marker, StringComparison.OrdinalIgnoreCase) >= 0) {
              return true;
            }
          }
        }
      } catch {
      }

      return false;
    }

    private static bool LogContainsMarker(string logPath, string marker) {
      if (string.IsNullOrWhiteSpace(logPath) || string.IsNullOrWhiteSpace(marker) || !File.Exists(logPath)) {
        return false;
      }

      try {
        return File.ReadLines(logPath).Any(line =>
          !string.IsNullOrWhiteSpace(line)
          && line.IndexOf(marker, StringComparison.OrdinalIgnoreCase) >= 0);
      } catch {
        return false;
      }
    }

    private static bool TryRepairBustedMsiRegistration(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds,
      string context,
      out string recoveryDetail) {
      recoveryDetail = string.Empty;
      if (!ShouldRepairBustedMsiRegistration(failureResult, allowedKinds)) {
        return false;
      }

      var targets = BuildMsiRegistrationRecoveryTargets(failureResult, allowedKinds);
      if (targets.Count == 0) {
        AppendInstallerLogMessage(
          failureResult.LogPath,
          "MSI registration repair was considered for " + (context ?? "install")
          + ", but no validated Vibeshine/Vibepollo product registrations were found.");
        return false;
      }

      AppendInstallerLogMessage(
        failureResult.LogPath,
        "Detected a recoverable previous MSI failure during " + (context ?? "install")
        + " (exit code " + failureResult.ExitCode + "). Attempting guarded Vibeshine/Vibepollo MSI registration repair.");

      TryStopRelatedServicesAndProcesses(failureResult.LogPath);
      var cleanupResult = CleanupMsiRegistrations(targets, failureResult.LogPath);
      if (cleanupResult.Succeeded || cleanupResult.RemovedItems > 0) {
        recoveryDetail = "Recovered from a previous MSI failure by removing "
          + cleanupResult.RemovedItems
          + " stale registry item(s) for "
          + BuildRecoveryTargetSummary(targets)
          + ".";
        if (cleanupResult.Errors.Count > 0) {
          recoveryDetail += " Some registry items could not be removed: " + string.Join("; ", cleanupResult.Errors.Take(3));
        }
        AppendInstallerLogMessage(failureResult.LogPath, recoveryDetail);
        return true;
      }

      recoveryDetail = "MSI registration repair found "
        + cleanupResult.TargetCount
        + " validated target(s), but did not remove any stale registry items.";
      if (cleanupResult.Errors.Count > 0) {
        recoveryDetail += " Errors: " + string.Join("; ", cleanupResult.Errors.Take(3));
      }
      AppendInstallerLogMessage(failureResult.LogPath, recoveryDetail);
      return false;
    }

    private static List<InstalledProductInfo> BuildMsiRegistrationRecoveryTargets(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds) {
      var targets = new List<InstalledProductInfo>();
      var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      var allowed = allowedKinds == null || allowedKinds.Count == 0
        ? MsiRegistrationRecoveryKinds
        : allowedKinds;
      var hasSpecificFailureProduct = failureResult != null
        && (!string.IsNullOrWhiteSpace(failureResult.ProductCode)
          || failureResult.ProductKind != InstalledProductKind.Unknown
          || !string.IsNullOrWhiteSpace(failureResult.ProductDisplayName));
      var hasConcreteFailureProduct = HasConcreteMsiRegistrationRecoveryProduct(failureResult, allowed);

      if (hasConcreteFailureProduct) {
        AddMsiRegistrationRecoveryTarget(targets, seen, new InstalledProductInfo {
          ProductCode = NormalizeProductCode(failureResult.ProductCode),
          DisplayName = failureResult.ProductDisplayName ?? string.Empty,
          Kind = failureResult.ProductKind,
          IsWindowsInstaller = true
        });
      }
      if (hasSpecificFailureProduct) {
        return targets;
      }

      var installedTargets = GetInstalledProductRegistrations(false)
        .Where(product =>
          product.IsWindowsInstaller
          && LooksLikeProductCode(product.ProductCode)
          && IsRecoveryKindAllowed(product.Kind, allowed))
        .ToList();
      foreach (var product in installedTargets) {
        AddMsiRegistrationRecoveryTarget(targets, seen, product);
      }

      return targets;
    }

    private static bool HasConcreteMsiRegistrationRecoveryProduct(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds) {
      return failureResult != null
        && IsRecoveryKindAllowed(failureResult.ProductKind, allowedKinds)
        && LooksLikeProductCode(failureResult.ProductCode);
    }

    private static InstalledProductInfo TryGetUnambiguousMsiRegistrationRecoveryProduct(
      InstalledProductKind productKind) {
      if (!IsRecoveryKindAllowed(productKind, MsiRegistrationRecoveryKinds)) {
        return null;
      }

      // Ambiguity must be measured over products Windows Installer actually
      // owns.  A single orphaned uninstall key left behind by an earlier failed
      // uninstall - precisely the machine state this recovery exists for -
      // would otherwise look like a second product and silently disable
      // recovery.  Genuinely ambiguous machines still fail closed because two
      // MSI-installed products both survive this filter.
      var candidates = GetInstalledProducts(false)
        .Where(product =>
          product.Kind == productKind
          && product.IsWindowsInstaller
          && !product.IsPerUser
          && LooksLikeProductCode(product.ProductCode)
          && IsInstalledProductCode(product.ProductCode))
        .ToList();
      return candidates.Count == 1 ? candidates[0] : null;
    }

    private static void AttachMsiRegistrationRecoveryProduct(
      InstallerResult result,
      InstalledProductInfo product) {
      if (result == null || product == null) {
        return;
      }

      var productCode = NormalizeProductCode(product.ProductCode);
      if (!LooksLikeProductCode(productCode)
          || !IsRecoveryKindAllowed(product.Kind, MsiRegistrationRecoveryKinds)) {
        return;
      }

      result.ProductCode = productCode;
      result.ProductDisplayName = product.DisplayName ?? string.Empty;
      result.ProductKind = product.Kind;
    }

    private static void AddMsiRegistrationRecoveryTarget(
      List<InstalledProductInfo> targets,
      HashSet<string> seen,
      InstalledProductInfo product) {
      if (targets == null || seen == null || product == null) {
        return;
      }
      var productCode = NormalizeProductCode(product.ProductCode);
      if (!LooksLikeProductCode(productCode)) {
        return;
      }
      if (product.Kind != InstalledProductKind.Vibeshine && product.Kind != InstalledProductKind.Vibepollo) {
        return;
      }
      if (seen.Contains(productCode)) {
        return;
      }
      seen.Add(productCode);
      product.ProductCode = productCode;
      targets.Add(product);
    }

    private static bool IsRecoveryKindAllowed(
      InstalledProductKind kind,
      IReadOnlyCollection<InstalledProductKind> allowedKinds) {
      if (kind != InstalledProductKind.Vibeshine && kind != InstalledProductKind.Vibepollo) {
        return false;
      }
      if (allowedKinds == null || allowedKinds.Count == 0) {
        return true;
      }
      return allowedKinds.Contains(kind);
    }

    private static string NormalizeProductCode(string productCode) {
      var value = (productCode ?? string.Empty).Trim();
      if (value.Length == 36) {
        value = "{" + value + "}";
      }
      return value.ToUpperInvariant();
    }

    internal static bool TryNormalizeSortableProductCode(string productCode, out string normalizedProductCode) {
      normalizedProductCode = string.Empty;

      Guid parsed;
      if (!Guid.TryParse(productCode, out parsed)) {
        return false;
      }

      // Compare canonical UUID text/hex, not Guid.CompareTo() or
      // Guid.ToByteArray(), because those use Windows GUID byte ordering and
      // would destroy the timestamp-first sort order used by UUIDv7.
      var canonicalHex = parsed.ToString("N").ToUpperInvariant();
      if (canonicalHex.Length != 32) {
        return false;
      }

      // Only UUIDv7 ProductCodes generated by the current packaging path are
      // sortable.  Legacy/random ProductCodes are not safely ordered.
      if (canonicalHex[12] != '7') {
        return false;
      }
      var variant = canonicalHex[16];
      if (variant != '8' && variant != '9' && variant != 'A' && variant != 'B') {
        return false;
      }

      normalizedProductCode = canonicalHex;
      return true;
    }

    private static string BuildRecoveryTargetSummary(IReadOnlyCollection<InstalledProductInfo> targets) {
      if (targets == null || targets.Count == 0) {
        return "no products";
      }
      return string.Join(
        ", ",
        targets.Select(product =>
          (string.IsNullOrWhiteSpace(product.DisplayName) ? product.Kind.ToString() : product.DisplayName)
          + " "
          + product.ProductCode));
    }

    private static void AppendRecoveryDetails(InstallerResult result, IReadOnlyCollection<string> recoveryDetails) {
      if (result == null || recoveryDetails == null || recoveryDetails.Count == 0) {
        return;
      }
      var detail = string.Join(" ", recoveryDetails.Where(item => !string.IsNullOrWhiteSpace(item)));
      if (string.IsNullOrWhiteSpace(detail)) {
        return;
      }
      result.Message = string.IsNullOrWhiteSpace(result.Message)
        ? detail
        : result.Message + " " + detail;
      if (string.IsNullOrWhiteSpace(result.UserDetail)) {
        result.UserDetail = detail;
      } else {
        result.UserDetail += "\n" + detail;
      }
      AppendInstallerLogMessage(result.LogPath, detail);
    }

    private static void AppendInstallerLogMessage(string logPath, string message) {
      if (string.IsNullOrWhiteSpace(logPath) || string.IsNullOrWhiteSpace(message)) {
        return;
      }
      try {
        var encoding = DetectTextFileEncodingForAppend(logPath);
        using (var writer = new StreamWriter(logPath, true, encoding)) {
          writer.WriteLine();
          writer.Write("[Vibepollo Bootstrapper ");
          writer.Write(DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss"));
          writer.Write(" UTC] ");
          writer.WriteLine(message);
        }
      } catch {
      }
    }

    private static Encoding DetectTextFileEncodingForAppend(string path) {
      try {
        if (!string.IsNullOrWhiteSpace(path) && File.Exists(path)) {
          var preamble = new byte[4];
          int bytesRead;
          using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            bytesRead = stream.Read(preamble, 0, preamble.Length);
          }
          if (bytesRead >= 2) {
            if (preamble[0] == 0xFF && preamble[1] == 0xFE) {
              return Encoding.Unicode;
            }
            if (preamble[0] == 0xFE && preamble[1] == 0xFF) {
              return Encoding.BigEndianUnicode;
            }
          }
          if (bytesRead >= 3 && preamble[0] == 0xEF && preamble[1] == 0xBB && preamble[2] == 0xBF) {
            return Encoding.UTF8;
          }
          if (bytesRead >= 4
              && preamble[0] == 0x00
              && preamble[1] == 0x00
              && preamble[2] == 0xFE
              && preamble[3] == 0xFF) {
            return Encoding.UTF32;
          }
        }
      } catch {
      }

      // Windows Installer verbose logs are UTF-16LE on modern Windows.
      return Encoding.Unicode;
    }

    private static void TryStopRelatedServicesAndProcesses(string logPath) {
      foreach (var serviceName in RelatedServiceNames.Distinct(StringComparer.OrdinalIgnoreCase)) {
        try {
          var scPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "sc.exe");
          var exitCode = RunProcess(scPath, "stop " + QuoteArgument(serviceName), true, false);
          AppendInstallerLogMessage(logPath, "Requested service stop for " + serviceName + " (exit code " + exitCode + ").");
        } catch (Exception ex) {
          AppendInstallerLogMessage(logPath, "Unable to request service stop for " + serviceName + ": " + ex.Message);
        }
      }

      Thread.Sleep(1000);

      foreach (var processName in RelatedProcessNames.Distinct(StringComparer.OrdinalIgnoreCase)) {
        try {
          foreach (var process in Process.GetProcessesByName(processName)) {
            using (process) {
              try {
                if (process.Id == Process.GetCurrentProcess().Id) {
                  continue;
                }
                if (!process.HasExited) {
                  process.CloseMainWindow();
                  process.WaitForExit(1500);
                }
                if (!process.HasExited) {
                  process.Kill();
                  process.WaitForExit(5000);
                }
                AppendInstallerLogMessage(logPath, "Stopped process " + processName + " (PID " + process.Id + ").");
              } catch (Exception ex) {
                AppendInstallerLogMessage(logPath, "Unable to stop process " + processName + " (PID " + process.Id + "): " + ex.Message);
              }
            }
          }
        } catch (Exception ex) {
          AppendInstallerLogMessage(logPath, "Unable to enumerate process " + processName + ": " + ex.Message);
        }
      }
    }

    private static MsiRegistrationCleanupResult CleanupMsiRegistrations(
      IReadOnlyCollection<InstalledProductInfo> targets,
      string logPath) {
      var result = new MsiRegistrationCleanupResult();
      if (targets == null || targets.Count == 0) {
        return result;
      }

      result.TargetCount = targets.Count;
      foreach (var target in targets) {
        var productCode = NormalizeProductCode(target.ProductCode);
        if (!LooksLikeProductCode(productCode)) {
          continue;
        }

        var packedProductCode = PackMsiGuid(productCode);
        if (string.IsNullOrWhiteSpace(packedProductCode)) {
          continue;
        }

        DeleteRegistrySubKeyAcrossViews(
          @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
          productCode,
          "ARP uninstall entry",
          result,
          logPath);
        DeleteRegistrySubKeyAcrossViews(
          @"SOFTWARE\Classes\Installer\Products",
          packedProductCode,
          "MSI Products entry",
          result,
          logPath);
        DeleteRegistrySubKeyAcrossViews(
          @"SOFTWARE\Classes\Installer\Features",
          packedProductCode,
          "MSI Features entry",
          result,
          logPath);
        DeleteRegistrySubKeyAcrossViews(
          @"SOFTWARE\Microsoft\Installer\Products",
          packedProductCode,
          "per-user MSI Products entry",
          result,
          logPath);
        DeleteRegistrySubKeyAcrossViews(
          @"SOFTWARE\Microsoft\Installer\Features",
          packedProductCode,
          "per-user MSI Features entry",
          result,
          logPath);
        DeleteUserDataProductRegistrations(packedProductCode, result, logPath);
        DeleteUpgradeCodeProductReferences(packedProductCode, result, logPath);
      }

      return result;
    }

    private static bool UserDataProductRegistrationExists(
      RegistryHive hive,
      RegistryView view,
      string sid,
      string packedProductCode) {
      if (string.IsNullOrWhiteSpace(sid) || string.IsNullOrWhiteSpace(packedProductCode)) {
        return false;
      }

      try {
        using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
        using (var productKey = baseKey.OpenSubKey(
          @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\" + sid + @"\Products\" + packedProductCode,
          false)) {
          if (productKey != null) {
            return true;
          }
        }
      } catch {
      }

      return false;
    }

    private static int CleanupStaleComponentClientsForInstallLocation(string installLocation, string logPath) {
      var normalizedInstallLocation = NormalizePath(installLocation);
      if (string.IsNullOrWhiteSpace(normalizedInstallLocation) || !Directory.Exists(normalizedInstallLocation)) {
        return 0;
      }

      var removed = 0;
      foreach (var hive in GetRegistryCleanupHives()) {
        foreach (var view in GetRegistryCleanupViews()) {
          try {
            using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
            using (var userDataRoot = baseKey.OpenSubKey(
              @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData",
              false)) {
              if (userDataRoot == null) {
                continue;
              }

              foreach (var sid in userDataRoot.GetSubKeyNames()) {
                var componentsPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\" + sid + @"\Components";
                using (var componentsRoot = baseKey.OpenSubKey(componentsPath, true)) {
                  if (componentsRoot == null) {
                    continue;
                  }

                  foreach (var componentKeyName in componentsRoot.GetSubKeyNames()) {
                    using (var componentKey = componentsRoot.OpenSubKey(componentKeyName, true)) {
                      if (componentKey == null) {
                        continue;
                      }

                      foreach (var clientProductCode in componentKey.GetValueNames()) {
                        if (string.IsNullOrWhiteSpace(clientProductCode)
                            || UserDataProductRegistrationExists(hive, view, sid, clientProductCode)) {
                          continue;
                        }

                        var componentPath = Convert.ToString(componentKey.GetValue(clientProductCode)) ?? string.Empty;
                        if (!PathIsUnderDirectory(componentPath, normalizedInstallLocation)) {
                          continue;
                        }

                        componentKey.DeleteValue(clientProductCode, false);
                        removed++;
                        AppendInstallerLogMessage(
                          logPath,
                          "Removed stale MSI component client " + clientProductCode
                          + " for component " + componentKeyName
                          + " pointing at " + componentPath + ".");
                      }
                    }

                    TryDeleteRegistrySubKeyIfEmpty(
                      hive,
                      view,
                      componentsPath,
                      componentKeyName,
                      null,
                      logPath);
                  }
                }
              }
            }
          } catch (Exception ex) {
            AppendInstallerLogMessage(
              logPath,
              "Unable to inspect stale MSI component clients in " + hive + " " + view + ": " + ex.Message);
          }
        }
      }

      if (removed > 0) {
        AppendInstallerLogMessage(
          logPath,
          "Removed " + removed + " stale MSI component client(s) under " + normalizedInstallLocation + " before uninstall.");
      }

      return removed;
    }

    private static void DeleteUserDataProductRegistrations(
      string packedProductCode,
      MsiRegistrationCleanupResult result,
      string logPath) {
      foreach (var hive in GetRegistryCleanupHives()) {
        foreach (var view in GetRegistryCleanupViews()) {
          try {
            using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
            using (var userDataRoot = baseKey.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData", false)) {
              if (userDataRoot == null) {
                continue;
              }
              foreach (var sid in userDataRoot.GetSubKeyNames()) {
                DeleteRegistrySubKey(
                  hive,
                  view,
                  @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\" + sid + @"\Products",
                  packedProductCode,
                  "MSI UserData Products entry",
                  result,
                  logPath);
                DeleteRegistrySubKey(
                  hive,
                  view,
                  @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\" + sid + @"\Features",
                  packedProductCode,
                  "MSI UserData Features entry",
                  result,
                  logPath);
              }
            }
          } catch (Exception ex) {
            AddCleanupError(result, "Unable to inspect MSI UserData in " + hive + " " + view + ": " + ex.Message);
          }
        }
      }
    }

    private static void DeleteUpgradeCodeProductReferences(
      string packedProductCode,
      MsiRegistrationCleanupResult result,
      string logPath) {
      var roots = new[] {
        @"SOFTWARE\Classes\Installer\UpgradeCodes",
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UpgradeCodes",
        @"SOFTWARE\Microsoft\Installer\UpgradeCodes"
      };
      foreach (var root in roots) {
        foreach (var hive in GetRegistryCleanupHives()) {
          foreach (var view in GetRegistryCleanupViews()) {
            try {
              using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
              using (var upgradeRoot = baseKey.OpenSubKey(root, true)) {
                if (upgradeRoot == null) {
                  continue;
                }
                foreach (var upgradeCodeKeyName in upgradeRoot.GetSubKeyNames()) {
                  using (var upgradeKey = upgradeRoot.OpenSubKey(upgradeCodeKeyName, true)) {
                    if (upgradeKey == null) {
                      continue;
                    }
                    if (!upgradeKey.GetValueNames().Contains(packedProductCode, StringComparer.OrdinalIgnoreCase)) {
                      continue;
                    }
                    upgradeKey.DeleteValue(packedProductCode, false);
                    result.RemovedItems++;
                    AppendInstallerLogMessage(
                      logPath,
                      "Removed MSI UpgradeCodes value " + packedProductCode + " from "
                      + hive + "\\" + root + "\\" + upgradeCodeKeyName + " (" + view + ").");
                  }

                  TryDeleteRegistrySubKeyIfEmpty(hive, view, root, upgradeCodeKeyName, result, logPath);
                }
              }
            } catch (Exception ex) {
              AddCleanupError(result, "Unable to inspect MSI UpgradeCodes in " + hive + "\\" + root + " (" + view + "): " + ex.Message);
            }
          }
        }
      }
    }

    private static void DeleteRegistrySubKeyAcrossViews(
      string parentPath,
      string subKeyName,
      string description,
      MsiRegistrationCleanupResult result,
      string logPath) {
      foreach (var hive in GetRegistryCleanupHives()) {
        foreach (var view in GetRegistryCleanupViews()) {
          DeleteRegistrySubKey(hive, view, parentPath, subKeyName, description, result, logPath);
        }
      }
    }

    private static void DeleteRegistrySubKey(
      RegistryHive hive,
      RegistryView view,
      string parentPath,
      string subKeyName,
      string description,
      MsiRegistrationCleanupResult result,
      string logPath) {
      if (string.IsNullOrWhiteSpace(parentPath) || string.IsNullOrWhiteSpace(subKeyName)) {
        return;
      }

      try {
        using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
        using (var parentKey = baseKey.OpenSubKey(parentPath, true)) {
          if (parentKey == null) {
            return;
          }
          if (!parentKey.GetSubKeyNames().Contains(subKeyName, StringComparer.OrdinalIgnoreCase)) {
            return;
          }
          parentKey.DeleteSubKeyTree(subKeyName, false);
          if (result != null) {
            result.RemovedItems++;
          }
          AppendInstallerLogMessage(
            logPath,
            "Removed " + description + " " + hive + "\\" + parentPath + "\\" + subKeyName + " (" + view + ").");
        }
      } catch (Exception ex) {
        AddCleanupError(
          result,
          "Unable to remove " + description + " " + hive + "\\" + parentPath + "\\" + subKeyName + " (" + view + "): " + ex.Message);
      }
    }

    private static void TryDeleteRegistrySubKeyIfEmpty(
      RegistryHive hive,
      RegistryView view,
      string parentPath,
      string subKeyName,
      MsiRegistrationCleanupResult result,
      string logPath) {
      try {
        using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
        using (var parentKey = baseKey.OpenSubKey(parentPath, true))
        using (var childKey = parentKey == null ? null : parentKey.OpenSubKey(subKeyName, false)) {
          if (parentKey == null || childKey == null) {
            return;
          }
          if (childKey.GetValueNames().Length > 0 || childKey.GetSubKeyNames().Length > 0) {
            return;
          }
          parentKey.DeleteSubKeyTree(subKeyName, false);
          if (result != null) {
            result.RemovedItems++;
          }
          AppendInstallerLogMessage(
            logPath,
            "Removed empty MSI UpgradeCodes key " + hive + "\\" + parentPath + "\\" + subKeyName + " (" + view + ").");
        }
      } catch {
      }
    }

    private static IEnumerable<RegistryHive> GetRegistryCleanupHives() {
      yield return RegistryHive.LocalMachine;
      yield return RegistryHive.CurrentUser;
    }

    private static IEnumerable<RegistryView> GetRegistryCleanupViews() {
      if (Environment.Is64BitOperatingSystem) {
        yield return RegistryView.Registry64;
        yield return RegistryView.Registry32;
      } else {
        yield return RegistryView.Default;
      }
    }

    private static void AddCleanupError(MsiRegistrationCleanupResult result, string message) {
      if (result == null || string.IsNullOrWhiteSpace(message)) {
        return;
      }
      if (result.Errors.Count < 10) {
        result.Errors.Add(message);
      }
    }

    private static string PackMsiGuid(string guidText) {
      Guid parsed;
      if (!Guid.TryParse(guidText, out parsed)) {
        return string.Empty;
      }

      var value = parsed.ToString("D").ToUpperInvariant();
      var parts = value.Split('-');
      if (parts.Length != 5) {
        return string.Empty;
      }

      return ReverseString(parts[0])
        + ReverseString(parts[1])
        + ReverseString(parts[2])
        + ReversePairs(parts[3])
        + ReversePairs(parts[4]);
    }

    private static string ReverseString(string value) {
      if (string.IsNullOrEmpty(value)) {
        return string.Empty;
      }
      var chars = value.ToCharArray();
      Array.Reverse(chars);
      return new string(chars);
    }

    private static string ReversePairs(string value) {
      if (string.IsNullOrEmpty(value)) {
        return string.Empty;
      }
      var builder = new StringBuilder(value.Length);
      for (var index = 0; index + 1 < value.Length; index += 2) {
        builder.Append(value[index + 1]);
        builder.Append(value[index]);
      }
      return builder.ToString();
    }

    private static bool TrySplitExecutableAndArguments(string commandLine, out string executablePath, out string arguments) {
      executablePath = string.Empty;
      arguments = string.Empty;
      var value = Environment.ExpandEnvironmentVariables(commandLine ?? string.Empty).Trim();
      if (value.Length == 0) {
        return false;
      }

      if (value.StartsWith("\"", StringComparison.Ordinal)) {
        var closingQuote = value.IndexOf('"', 1);
        if (closingQuote <= 1) {
          return false;
        }

        executablePath = value.Substring(1, closingQuote - 1).Trim();
        arguments = value.Substring(closingQuote + 1).Trim();
        return executablePath.Length > 0;
      }

      var exeIndex = value.IndexOf(".exe", StringComparison.OrdinalIgnoreCase);
      if (exeIndex > 0) {
        executablePath = value.Substring(0, exeIndex + 4).Trim();
        arguments = value.Substring(exeIndex + 4).Trim();
        return executablePath.Length > 0;
      }

      var firstSpace = value.IndexOfAny(new[] { ' ', '\t' });
      if (firstSpace < 0) {
        executablePath = value;
        return true;
      }

      executablePath = value.Substring(0, firstSpace).Trim();
      arguments = value.Substring(firstSpace + 1).Trim();
      return executablePath.Length > 0;
    }

    private static string BuildSilentUninstallCommand(InstalledProductInfo product) {
      var commandLine = string.IsNullOrWhiteSpace(product == null ? null : product.QuietUninstallString)
        ? (product == null ? string.Empty : product.UninstallString)
        : product.QuietUninstallString;
      if (string.IsNullOrWhiteSpace(commandLine)) {
        return string.Empty;
      }

      var normalized = commandLine.Trim();
      if (!string.IsNullOrWhiteSpace(product == null ? null : product.QuietUninstallString)) {
        return normalized;
      }
      if (CommandTargetsMsiexec(normalized)) {
        if (!HasCommandToken(normalized, "/qn") && !HasCommandToken(normalized, "/quiet")) {
          normalized += " /qn";
        }
        if (!HasCommandToken(normalized, "/norestart")) {
          normalized += " /norestart";
        }
        if (!ContainsPropertyAssignment(normalized, "REBOOT")) {
          normalized += " REBOOT=ReallySuppress";
        }
        if (!ContainsPropertyAssignment(normalized, "SUPPRESSMSGBOXES")) {
          normalized += " SUPPRESSMSGBOXES=1";
        }
        return normalized;
      }
      if (!HasQuietCommandSwitch(normalized)) {
        normalized += " /S";
      }
      return normalized;
    }

    private static bool HasQuietCommandSwitch(string commandLine) {
      return HasCommandToken(commandLine, "/s")
        || HasCommandToken(commandLine, "/silent")
        || HasCommandToken(commandLine, "/verysilent")
        || HasCommandToken(commandLine, "/quiet")
        || HasCommandToken(commandLine, "/qn");
    }

    private static bool HasCommandToken(string commandLine, string token) {
      var expanded = " " + Environment.ExpandEnvironmentVariables(commandLine ?? string.Empty).ToUpperInvariant() + " ";
      return expanded.IndexOf(" " + token.ToUpperInvariant() + " ", StringComparison.Ordinal) >= 0;
    }

    private static bool ContainsPropertyAssignment(string commandLine, string propertyName) {
      return Environment.ExpandEnvironmentVariables(commandLine ?? string.Empty)
        .IndexOf(propertyName + "=", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static bool CommandTargetsMsiexec(string commandLine) {
      string executablePath;
      string arguments;
      if (!TrySplitExecutableAndArguments(commandLine, out executablePath, out arguments)) {
        return false;
      }

      return IsMsiexecExecutable(executablePath);
    }
    private static bool IsMsiexecExecutable(string executablePath) {
      if (string.IsNullOrWhiteSpace(executablePath)) {
        return false;
      }
      var fileName = Path.GetFileName(executablePath.Trim().Trim('"'));
      return string.Equals(fileName, "msiexec.exe", StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileName, "msiexec", StringComparison.OrdinalIgnoreCase);
    }

    private static bool HasQuietUninstallSwitch(string uninstallArguments) {
      if (string.IsNullOrWhiteSpace(uninstallArguments)) {
        return false;
      }

      var tokens = uninstallArguments
        .Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)
        .Select(token => token.Trim('"', '\''))
        .ToArray();
      return tokens.Any(token =>
        token.Equals("/quiet", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/qn", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/qb", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/passive", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/s", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/silent", StringComparison.OrdinalIgnoreCase));
    }

    public static InstallerResult RunInteractiveUninstall(
      InstallerArguments arguments,
      bool factoryResetAppData = false,
      bool removeVirtualDisplayDriver = false,
      bool allowSelfElevation = true) {
      if (arguments != null && !string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "MSI overrides are not accepted for uninstall. Use the registered ProductCode."
        };
      }
      if (allowSelfElevation && !IsProcessElevated()) {
        return RunElevatedBootstrapperUninstall(arguments, factoryResetAppData, removeVirtualDisplayDriver);
      }

      SweepStaleInstallerRecoveryDirectories();

      var uninstallResult = UninstallInstalledProducts(
        "uninstall",
        true,
        false,
        factoryResetAppData,
        removeVirtualDisplayDriver,
        true,
        new[] { InstalledProductKind.Vibepollo });
      uninstallResult.Operation = InstallerOperation.Uninstall;
      return uninstallResult;
    }

    public static InstallerResult RunCli(InstallerArguments arguments) {
      string payloadFailure;
      if (RejectUntrustedCliPayloadRequest(
            arguments,
            IsProcessElevated(),
            out payloadFailure)) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = payloadFailure
        };
      }

      if (!IsRegisteredMaintenanceOperation(arguments.ForwardedArguments)) {
        return RunCliWithPinnedMaintenancePayload(arguments, null);
      }

      try {
        using (var pinnedMaintenancePayload =
          OpenPinnedRegisteredMaintenancePayload(arguments.ForwardedArguments)) {
          if (pinnedMaintenancePayload == null) {
            throw new InvalidDataException(
              "The registered MSI cache could not be authenticated and pinned.");
          }
          return RunCliWithPinnedMaintenancePayload(arguments, pinnedMaintenancePayload);
        }
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = IsMsiUninstallOperation(arguments.ForwardedArguments)
            ? InstallerOperation.Uninstall
            : InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The registered MSI cache is not trusted: " + ex.Message
        };
      }
    }

    private static InstallerResult RunCliWithPinnedMaintenancePayload(
      InstallerArguments arguments,
      PinnedMsiPayload pinnedMaintenancePayload) {
      var cliArgs = new List<string>(arguments.ForwardedArguments);
      var hasOperation = cliArgs.Any(IsOperationSwitch);
      var installMsiPath = string.Empty;
      string injectedMsiPath = null;
      var recoveryDetails = new List<string>();

      SweepStaleInstallerRecoveryDirectories();

      if (!IsProcessElevated()
          && string.IsNullOrWhiteSpace(arguments.MsiPathOverride)
          && ShouldElevateCliForEmbeddedPayload(cliArgs, hasOperation)) {
        var requestedInstallRoot = GetPropertyValue(cliArgs, "INSTALL_ROOT");
        if (!string.IsNullOrWhiteSpace(requestedInstallRoot)) {
          try {
            NormalizeInstallPath(requestedInstallRoot);
          } catch (Exception ex) {
            return new InstallerResult {
              Operation = InstallerOperation.Install,
              ExitCode = 1603,
              Message = "The install location is not trusted: " + ex.Message
            };
          }
        }
        return RunElevatedBootstrapperCli(arguments);
      }

      try {
        if (!hasOperation) {
          installMsiPath = ResolveMsiPath(arguments.MsiPathOverride);
          injectedMsiPath = installMsiPath;
          cliArgs.Insert(0, installMsiPath);
          cliArgs.Insert(0, "/i");
        } else {
          injectedMsiPath = TryInjectDefaultMsi(cliArgs, arguments);
        }
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = IsMsiUninstallOperation(cliArgs)
            ? InstallerOperation.Uninstall
            : InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The installer could not resolve a valid MSI payload: " + ex.Message
        };
      }

      string operationTargetFailure;
      if (!TryNormalizeAndValidateMsiOperationTarget(
        cliArgs,
        out operationTargetFailure)) {
        return new InstallerResult {
          Operation = IsMsiUninstallOperation(cliArgs)
            ? InstallerOperation.Uninstall
            : InstallerOperation.Install,
          ExitCode = 1603,
          Message = operationTargetFailure
        };
      }
      var isInstallOperation = IsMsiInstallOperation(cliArgs);
      var isUninstallOperation = IsMsiUninstallOperation(cliArgs);
      if (isInstallOperation) {
        var operationIndex = cliArgs.FindIndex(IsOperationSwitch);
        var explicitPath = operationIndex >= 0 && operationIndex + 1 < cliArgs.Count
          ? cliArgs[operationIndex + 1]
          : null;
        if (!string.IsNullOrWhiteSpace(explicitPath)) {
          var fullExplicitPath = Path.GetFullPath(explicitPath);
          string approvedHash;
          lock (ApprovedPayloadLock) {
            ApprovedPayloadHashes.TryGetValue(fullExplicitPath, out approvedHash);
          }
          if (string.IsNullOrWhiteSpace(approvedHash)) {
            try {
              var stagedPath = StageDeveloperMsiOverride(fullExplicitPath);
              cliArgs[operationIndex + 1] = stagedPath;
              injectedMsiPath = stagedPath;
            } catch (Exception ex) {
              return new InstallerResult {
                Operation = InstallerOperation.Install,
                ExitCode = 1603,
                Message = "The developer MSI did not match the embedded trust boundary: " + ex.Message
              };
            }
          }
        }
      }
      if (isInstallOperation) {
        var requestedInstallRoot = GetPropertyValue(cliArgs, "INSTALL_ROOT");
        if (!string.IsNullOrWhiteSpace(requestedInstallRoot)) {
          try {
            var normalizedInstallRoot = NormalizeInstallPath(requestedInstallRoot);
            var propertyIndex = cliArgs.FindIndex(
              arg => arg.StartsWith("INSTALL_ROOT=", StringComparison.OrdinalIgnoreCase));
            if (propertyIndex >= 0) {
              cliArgs[propertyIndex] = CreatePropertyArgument(
                "INSTALL_ROOT",
                normalizedInstallRoot);
            }
          } catch (Exception ex) {
            return new InstallerResult {
              Operation = InstallerOperation.Install,
              ExitCode = 1603,
              Message = "The install location is not trusted: " + ex.Message
            };
          }
        }
      }
      if (isInstallOperation) {
        installMsiPath = GetMsiPathArgument(cliArgs) ?? string.Empty;
      }
      var cliUninstallRecoveryProduct = isUninstallOperation
        ? TryResolveCliMsiUninstallRecoveryProduct(cliArgs)
        : null;
      if (!IsProcessElevated() && (isInstallOperation || isUninstallOperation)) {
        return RunElevatedBootstrapperCli(arguments, cliArgs);
      }

      if (!HasRestartBehavior(cliArgs)) {
        cliArgs.Add("/norestart");
      }
      if (!HasProperty(cliArgs, "REBOOT")) {
        cliArgs.Add("REBOOT=ReallySuppress");
      }
      if (!HasProperty(cliArgs, "SUPPRESSMSGBOXES")) {
        cliArgs.Add("SUPPRESSMSGBOXES=1");
      }
      PreserveCliVirtualDisplayDriverSelection(cliArgs);

      var logPath = TryGetMsiLogPath(cliArgs);
      if (!HasLogSwitch(cliArgs)) {
        logPath = BuildLogPath("cli");
        InsertMsiSwitchAfterOperation(cliArgs, "/l*v", logPath);
      }

      var uninstallCompetingProducts = ShouldPreUninstallCompetingProducts(cliArgs);
      var competingProductsRequireRestart = false;
      var vibeshineSourceRequiresRestart = false;
      StashedVibeshinePayload stashedPreviousPayload = null;
      if (uninstallCompetingProducts) {
        var uninstallCompetingProductsResult = UninstallCompetingProducts(
          "cli_remove_competing",
          arguments.IsCliQuietMode(),
          true);
        if (!uninstallCompetingProductsResult.Succeeded) {
          if (ShouldRerunCliElevatedForMsiRecovery(uninstallCompetingProductsResult, new[] { InstalledProductKind.Vibepollo })) {
            return RunElevatedBootstrapperCli(arguments);
          }
          string recoveryDetail;
          if (TryRepairBustedMsiRegistration(
            uninstallCompetingProductsResult,
            new[] { InstalledProductKind.Vibepollo },
            "CLI competing product pre-uninstall",
            out recoveryDetail)) {
            recoveryDetails.Add(recoveryDetail);
          } else {
            return new InstallerResult {
              Operation = InstallerOperation.Install,
              ExitCode = uninstallCompetingProductsResult.ExitCode,
              Message = BuildCompetingProductUninstallFailureMessage(uninstallCompetingProductsResult.Message),
              LogPath = uninstallCompetingProductsResult.LogPath
            };
          }
        }
        competingProductsRequireRestart = uninstallCompetingProductsResult.ExitCode == 3010;
      }

      if (ShouldPreUninstallProblematicUpgradeSource(cliArgs)) {
        StashedVibeshinePayload upgradeSourceStash;
        var uninstallUpgradeSourceResult = TryPreUninstallProblematicUpgradeSourceVersion(
          "cli_remove_vibepollo_1148",
          arguments.IsCliQuietMode(),
          true,
          out upgradeSourceStash);
        AdoptStashedPayload(ref stashedPreviousPayload, upgradeSourceStash);
        if (uninstallUpgradeSourceResult != null) {
          if (!uninstallUpgradeSourceResult.Succeeded) {
            if (ShouldRerunCliElevatedForMsiRecovery(uninstallUpgradeSourceResult, new[] { InstalledProductKind.Vibepollo })) {
              return ApplyStashedPayloadRecovery(
                RunElevatedBootstrapperCli(arguments),
                stashedPreviousPayload,
                "cli_restore_previous");
            }
            string recoveryDetail;
            if (TryRepairBustedMsiRegistration(
              uninstallUpgradeSourceResult,
              new[] { InstalledProductKind.Vibepollo },
              "CLI upgrade source pre-uninstall",
              out recoveryDetail)) {
              recoveryDetails.Add(recoveryDetail);
            } else {
              return ApplyStashedPayloadRecovery(new InstallerResult {
                Operation = InstallerOperation.Install,
                ExitCode = uninstallUpgradeSourceResult.ExitCode,
                Message = BuildUpgradeSourcePreUninstallFailureMessage(uninstallUpgradeSourceResult.Message),
                LogPath = uninstallUpgradeSourceResult.LogPath
              }, stashedPreviousPayload, "cli_restore_previous");
            }
          }
          vibeshineSourceRequiresRestart |= uninstallUpgradeSourceResult.ExitCode == 3010;
        }
      }
      if (uninstallCompetingProducts && !HasProperty(cliArgs, "SKIP_REMOVE_CONFLICTING_PRODUCTS")) {
        cliArgs.Add("SKIP_REMOVE_CONFLICTING_PRODUCTS=1");
      }

      if (ShouldPreUninstallVibeshineInstallSource(cliArgs)) {
        StashedVibeshinePayload downgradeStash;
        var uninstallDowngradeSourceResult = TryPreUninstallDowngradeSourceVersion(
          GetMsiPathArgument(cliArgs),
          "cli_remove_vibeshine_same_or_downgrade",
          arguments.IsCliQuietMode(),
          true,
          out downgradeStash);
        AdoptStashedPayload(ref stashedPreviousPayload, downgradeStash);
        if (uninstallDowngradeSourceResult != null) {
          if (!uninstallDowngradeSourceResult.Succeeded) {
            if (ShouldRerunCliElevatedForMsiRecovery(uninstallDowngradeSourceResult, new[] { InstalledProductKind.Vibeshine })) {
              return ApplyStashedPayloadRecovery(
                RunElevatedBootstrapperCli(arguments),
                stashedPreviousPayload,
                "cli_restore_previous");
            }
            string recoveryDetail;
            if (TryRepairBustedMsiRegistration(
              uninstallDowngradeSourceResult,
              new[] { InstalledProductKind.Vibeshine },
              "CLI same-version or downgrade source pre-uninstall",
              out recoveryDetail)) {
              recoveryDetails.Add(recoveryDetail);
            } else {
              return ApplyStashedPayloadRecovery(new InstallerResult {
                Operation = InstallerOperation.Install,
                ExitCode = uninstallDowngradeSourceResult.ExitCode,
                Message = BuildDowngradeSourcePreUninstallFailureMessage(uninstallDowngradeSourceResult.Message),
                LogPath = uninstallDowngradeSourceResult.LogPath
              }, stashedPreviousPayload, "cli_restore_previous");
            }
          }
          vibeshineSourceRequiresRestart |= uninstallDowngradeSourceResult.ExitCode == 3010;
        }
      }

      if (isInstallOperation && !string.IsNullOrWhiteSpace(installMsiPath)) {
        var migrationCleanupResult = RunPreinstallMigrationCleanup("preinstall_cli", arguments.IsCliQuietMode(), true);
        if (!migrationCleanupResult.Succeeded) {
          var migrationRecoveryKinds = new[] { InstalledProductKind.Vibeshine };
          if (ShouldRerunCliElevatedForMsiRecovery(migrationCleanupResult, migrationRecoveryKinds)) {
            return ApplyStashedPayloadRecovery(
              RunElevatedBootstrapperCli(arguments),
              stashedPreviousPayload,
              "cli_restore_previous");
          }
          string recoveryDetail;
          if (TryRepairBustedMsiRegistration(
            migrationCleanupResult,
            migrationRecoveryKinds,
            "CLI preinstall migration cleanup",
            out recoveryDetail)) {
            recoveryDetails.Add(recoveryDetail);
            migrationCleanupResult = RunPreinstallMigrationCleanup(
              "preinstall_cli_registration_recovery",
              arguments.IsCliQuietMode(),
              true);
          }
        }
        if (migrationCleanupResult.ExitCode != 0) {
          migrationCleanupResult.Operation = InstallerOperation.Install;
          migrationCleanupResult.InstallDeferredForRestart = migrationCleanupResult.ExitCode == 3010;
          if (migrationCleanupResult.InstallDeferredForRestart) {
            migrationCleanupResult.ExitCode = InstallDeferredRebootRequiredExitCode;
          }
          migrationCleanupResult.Message = migrationCleanupResult.InstallDeferredForRestart
            ? "Installation is deferred. " + migrationCleanupResult.Message
            : "Install could not continue. " + migrationCleanupResult.Message;
          AppendRecoveryDetails(migrationCleanupResult, recoveryDetails);
          return ApplyStashedPayloadRecovery(
            migrationCleanupResult,
            stashedPreviousPayload,
            "cli_restore_previous");
        }
        TryAppendSameProductReinstallProperties(cliArgs, installMsiPath);
      }

      AppendInstallerLogMessage(logPath, "Quiescing related services and helper processes before CLI MSI operation.");
      TryStopRelatedServicesAndProcesses(logPath);

      var registrationRecoveryProduct = isInstallOperation
        ? TryGetUnambiguousMsiRegistrationRecoveryProduct(InstalledProductKind.Vibepollo)
        : null;
      var exitCode = pinnedMaintenancePayload == null
        ? RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true)
        : RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true, pinnedMaintenancePayload);
      if (isInstallOperation) {
        exitCode = RetryInstallWithSameProductReinstallIfNeeded(
          exitCode,
          cliArgs,
          installMsiPath,
          arguments.IsCliQuietMode(),
          true);
      }
      if (!string.IsNullOrWhiteSpace(injectedMsiPath)
          && ShouldRetryInstallWithFreshPayload(arguments, injectedMsiPath, new InstallerResult {
            Operation = isUninstallOperation ? InstallerOperation.Uninstall : InstallerOperation.Install,
            ExitCode = exitCode,
            LogPath = logPath
          })) {
        TryDeleteFile(injectedMsiPath);
        string refreshedMsiPath;
        try {
          refreshedMsiPath = ResolveMsiPath(null, true);
        } catch (Exception ex) {
          var refreshFailure = new InstallerResult {
            Operation = isUninstallOperation ? InstallerOperation.Uninstall : InstallerOperation.Install,
            ExitCode = 1603,
            Message = "The installer could not extract a fresh MSI payload: " + ex.Message,
            LogPath = logPath
          };
          AppendRecoveryDetails(refreshFailure, recoveryDetails);
          return ApplyStashedPayloadRecovery(
            refreshFailure,
            stashedPreviousPayload,
            "cli_restore_previous");
        }
        var retryArgs = new List<string>(cliArgs);
        ReplaceArgumentValue(retryArgs, injectedMsiPath, refreshedMsiPath);
        if (!string.IsNullOrWhiteSpace(logPath)) {
          var retryLogPath = BuildLogPath("cli_recovery");
          if (ReplaceArgumentValue(retryArgs, logPath, retryLogPath)) {
            logPath = retryLogPath;
          }
        }

        cliArgs = retryArgs;
        installMsiPath = refreshedMsiPath;
        injectedMsiPath = refreshedMsiPath;
        exitCode = RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true);
        if (isInstallOperation) {
          exitCode = RetryInstallWithSameProductReinstallIfNeeded(
            exitCode,
            cliArgs,
            installMsiPath,
            arguments.IsCliQuietMode(),
            true);
        }
      }

      if (isUninstallOperation
          && cliUninstallRecoveryProduct != null
          && IsRecoverableMsiFirewallCleanupFailure(exitCode, logPath)) {
        var uninstallFailure = new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = exitCode,
          LogPath = logPath
        };
        AttachMsiRegistrationRecoveryProduct(uninstallFailure, cliUninstallRecoveryProduct);
        if (ShouldRerunCliElevatedForMsiRecovery(uninstallFailure, MsiRegistrationRecoveryKinds)) {
          return ApplyStashedPayloadRecovery(
            RunElevatedBootstrapperCli(arguments),
            stashedPreviousPayload,
            "cli_restore_previous");
        }

        int retryExitCode;
        string retryLogPath;
        string recoveryDetail;
        StashedVibeshinePayload ignoredStashedPayload;
        if (TryRunFirewallTolerantUninstall(
          cliUninstallRecoveryProduct,
          cliArgs,
          logPath,
          "cli_uninstall_firewall_cleanup_recovery",
          arguments.IsCliQuietMode(),
          true,
          false,
          out retryExitCode,
          out retryLogPath,
          out recoveryDetail,
          out ignoredStashedPayload)) {
          exitCode = retryExitCode;
          logPath = retryLogPath;
          recoveryDetails.Add(recoveryDetail);
        }
      }

      if (isInstallOperation) {
        var repairFailure = new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = exitCode,
          LogPath = logPath
        };
        AttachMsiRegistrationRecoveryProduct(repairFailure, registrationRecoveryProduct);
        if (ShouldRerunCliElevatedForMsiRecovery(repairFailure, MsiRegistrationRecoveryKinds)) {
          return ApplyStashedPayloadRecovery(
            RunElevatedBootstrapperCli(arguments),
            stashedPreviousPayload,
            "cli_restore_previous");
        }
        bool firewallRecoveryRequiresRestart;
        string firewallRecoveryDetail;
        StashedVibeshinePayload firewallRecoveryStash;
        if (TryRecoverMsiFirewallCleanupFailure(
          repairFailure,
          MsiRegistrationRecoveryKinds,
          "CLI install upgrade",
          arguments.IsCliQuietMode(),
          true,
          out firewallRecoveryRequiresRestart,
          out firewallRecoveryDetail,
          out firewallRecoveryStash)) {
          AdoptStashedPayload(ref stashedPreviousPayload, firewallRecoveryStash);
          recoveryDetails.Add(firewallRecoveryDetail);
          vibeshineSourceRequiresRestart |= firewallRecoveryRequiresRestart;
          var retryArgs = new List<string>(cliArgs);
          if (!string.IsNullOrWhiteSpace(logPath)) {
            var retryLogPath = BuildLogPath("cli_firewall_cleanup_recovery");
            if (ReplaceArgumentValue(retryArgs, logPath, retryLogPath)) {
              logPath = retryLogPath;
            }
          }
          cliArgs = retryArgs;
          exitCode = RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true);
          repairFailure = new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = exitCode,
            LogPath = logPath
          };
          AttachMsiRegistrationRecoveryProduct(repairFailure, registrationRecoveryProduct);
        }
        if (ShouldRepairBustedMsiRegistration(repairFailure, MsiRegistrationRecoveryKinds)) {
          if (ShouldRerunCliElevatedForMsiRecovery(repairFailure, MsiRegistrationRecoveryKinds)) {
            return ApplyStashedPayloadRecovery(
              RunElevatedBootstrapperCli(arguments),
              stashedPreviousPayload,
              "cli_restore_previous");
          }
          string recoveryDetail;
          if (TryRepairBustedMsiRegistration(
            repairFailure,
            MsiRegistrationRecoveryKinds,
            "CLI install upgrade",
            out recoveryDetail)) {
            recoveryDetails.Add(recoveryDetail);
            var retryArgs = new List<string>(cliArgs);
            if (!string.IsNullOrWhiteSpace(logPath)) {
              var retryLogPath = BuildLogPath("cli_registration_recovery");
              if (ReplaceArgumentValue(retryArgs, logPath, retryLogPath)) {
                logPath = retryLogPath;
              }
            }
            cliArgs = retryArgs;
            exitCode = RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true);
          }
        }
      }
      if (exitCode == 0 && (competingProductsRequireRestart || vibeshineSourceRequiresRestart || InstallLogIndicatesDriverRebootRequired(logPath))) {
        exitCode = 3010;
      }
      if ((exitCode == 0 || exitCode == 3010) && isInstallOperation) {
        var installedMsiPath = GetMsiPathArgument(cliArgs);
        string validationFailure;
        if (!string.IsNullOrWhiteSpace(installedMsiPath)
            && !ValidatePayloadRegisteredAfterInstall(installedMsiPath, logPath, out validationFailure)) {
          AppendInstallerLogMessage(logPath, validationFailure);
          exitCode = 1603;
        }
      }
      if (exitCode != 0 && exitCode != 3010) {
        TryRecoverServiceStateAfterFailedInstall();
      }
      var cliResult = new InstallerResult {
        Operation = isUninstallOperation ? InstallerOperation.Uninstall : InstallerOperation.Install,
        ExitCode = exitCode,
        Message = BuildResultMessage("CLI operation", exitCode, logPath),
        LogPath = logPath
      };
      AppendRecoveryDetails(cliResult, recoveryDetails);
      return ApplyStashedPayloadRecovery(cliResult, stashedPreviousPayload, "cli_restore_previous");
    }

    private static InstallerResult RunPreinstallMigrationCleanup(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded) {
      var migrationKinds = new HashSet<InstalledProductKind> {
        InstalledProductKind.Sunshine,
        InstalledProductKind.Vibeshine,
        InstalledProductKind.Apollo
      };
      var migrationRequired = GetInstalledProducts(true)
        .Any(product => migrationKinds.Contains(product.Kind))
        || GetLegacySunshineRegistration() != null
        || GetLegacyApolloRegistration() != null;
      return migrationRequired
        ? ManualLegacyRemovalRequired("Sunshine, Vibeshine, or Apollo detected during migration")
        : new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "No preinstall migration cleanup is required."
        };
    }


    private static readonly string[] PreinstallServiceNames = {
      "SunshineService",
      "sunshinesvc"
    };

    private static readonly string[] PostInstallServiceNames = {
      "SunshineService",
      "VibeshineService",
      "sunshinesvc"
    };

    private static readonly string[] PreinstallProcessNames = {
      "vibeshine",
      "sunshine",
      "sunshinesvc",
      "sunshine_audio_policy_helper",
      "apollo",
      "vibepollo"
    };

    private static void TryDrainPreinstallLocks() {
      foreach (var serviceName in PreinstallServiceNames) {
        TryStopServiceAndWait(serviceName);
      }

      foreach (var processName in PreinstallProcessNames) {
        TryKillProcessesByName(processName);
      }
    }

    private static void TryKillProcessesByName(string processName) {
      if (string.IsNullOrWhiteSpace(processName)) {
        return;
      }

      Process[] processes;
      try {
        processes = Process.GetProcessesByName(processName);
      } catch {
        return;
      }

      foreach (var process in processes) {
        try {
          if (process.HasExited) {
            continue;
          }
          process.Kill();
          process.WaitForExit(5000);
        } catch {
        } finally {
          process.Dispose();
        }
      }
    }

    private static void TryRunUtilityProcess(string executable, string arguments) {
      if (string.IsNullOrWhiteSpace(executable)) {
        return;
      }

      try {
        var startInfo = new ProcessStartInfo {
          FileName = executable,
          Arguments = arguments ?? string.Empty,
          UseShellExecute = false,
          CreateNoWindow = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return;
          }
          if (!process.WaitForExit(10000)) {
            try {
              process.Kill();
              process.WaitForExit(5000);
            } catch {
            }
          }
        }
      } catch {
      }
    }

    private static void TryStopServiceAndWait(string serviceName) {
      if (string.IsNullOrWhiteSpace(serviceName)) {
        return;
      }

      TryRunUtilityProcess("net.exe", "stop " + serviceName);
      WaitForServiceStateStopped(serviceName, TimeSpan.FromSeconds(15));
    }

    private static void TryRecoverServiceStateAfterFailedInstall() {
      foreach (var serviceName in PostInstallServiceNames) {
        TryStartServiceAndWait(serviceName);
      }
    }

    private static void TryStartServiceAndWait(string serviceName) {
      if (string.IsNullOrWhiteSpace(serviceName)) {
        return;
      }

      TryRunUtilityProcess("net.exe", "start " + serviceName);
      WaitForServiceStateRunning(serviceName, TimeSpan.FromSeconds(15));
    }

    private static void WaitForServiceStateStopped(string serviceName, TimeSpan timeout) {
      if (string.IsNullOrWhiteSpace(serviceName) || timeout <= TimeSpan.Zero) {
        return;
      }

      var deadline = DateTime.UtcNow + timeout;
      while (DateTime.UtcNow < deadline) {
        if (IsServiceStopped(serviceName)) {
          return;
        }
        System.Threading.Thread.Sleep(500);
      }
    }

    private static void WaitForServiceStateRunning(string serviceName, TimeSpan timeout) {
      if (string.IsNullOrWhiteSpace(serviceName) || timeout <= TimeSpan.Zero) {
        return;
      }

      var deadline = DateTime.UtcNow + timeout;
      while (DateTime.UtcNow < deadline) {
        if (IsServiceRunning(serviceName)) {
          return;
        }
        System.Threading.Thread.Sleep(500);
      }
    }

    private static bool IsServiceStopped(string serviceName) {
      try {
        var startInfo = new ProcessStartInfo {
          FileName = "sc.exe",
          Arguments = "query " + serviceName,
          UseShellExecute = false,
          CreateNoWindow = true,
          RedirectStandardOutput = true,
          RedirectStandardError = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return false;
          }
          var output = process.StandardOutput.ReadToEnd();
          process.WaitForExit(5000);
          return output.IndexOf("STATE", StringComparison.OrdinalIgnoreCase) >= 0 &&
                 output.IndexOf("STOPPED", StringComparison.OrdinalIgnoreCase) >= 0;
        }
      } catch {
        return false;
      }
    }

    private static bool IsServiceRunning(string serviceName) {
      try {
        var startInfo = new ProcessStartInfo {
          FileName = "sc.exe",
          Arguments = "query " + serviceName,
          UseShellExecute = false,
          CreateNoWindow = true,
          RedirectStandardOutput = true,
          RedirectStandardError = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return false;
          }
          var output = process.StandardOutput.ReadToEnd();
          process.WaitForExit(5000);
          return output.IndexOf("STATE", StringComparison.OrdinalIgnoreCase) >= 0 &&
                 output.IndexOf("RUNNING", StringComparison.OrdinalIgnoreCase) >= 0;
        }
      } catch {
        return false;
      }
    }

    private static void PreserveCliVirtualDisplayDriverSelection(List<string> cliArgs) {
      if (!IsMsiInstallOperation(cliArgs) || HasProperty(cliArgs, "INSTALL_VIRTUAL_DISPLAY_DRIVER")) {
        return;
      }

      bool useSunshineDriver;
      if (!TryReadCliSunshineVirtualDisplayDriverSelection(cliArgs, out useSunshineDriver)) {
        return;
      }

      cliArgs.Add("INSTALL_VIRTUAL_DISPLAY_DRIVER=" + (useSunshineDriver ? "1" : "0"));
    }

    private static bool IsMsiInstallOperation(List<string> cliArgs) {
      var operation = cliArgs == null ? null : cliArgs.FirstOrDefault(IsOperationSwitch);
      return string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
        || string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsMsiUninstallOperation(List<string> cliArgs) {
      var operation = cliArgs == null ? null : cliArgs.FirstOrDefault(IsOperationSwitch);
      return string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsMsiUninstallOperation(IReadOnlyList<string> cliArgs) {
      var operation = cliArgs == null ? null : cliArgs.FirstOrDefault(IsOperationSwitch);
      return string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsRegisteredMaintenanceOperation(IReadOnlyList<string> cliArgs) {
      var operation = cliArgs == null ? null : cliArgs.FirstOrDefault(IsOperationSwitch);
      return string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase)
        || string.Equals(operation, "/f", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryGetSoleMsiOperationTarget(
      IReadOnlyList<string> arguments,
      out int operationIndex,
      out string operation,
      out int targetIndex,
      out string target) {
      operationIndex = -1;
      operation = string.Empty;
      targetIndex = -1;
      target = string.Empty;
      if (arguments == null) {
        return false;
      }

      for (var index = 0; index < arguments.Count; index++) {
        var token = (arguments[index] ?? string.Empty).Trim().Trim('"');
        if (!IsOperationSwitch(token)) {
          continue;
        }
        if (operationIndex >= 0) {
          return false;
        }
        operationIndex = index;
        operation = token;
      }
      if (operationIndex < 0 || operationIndex + 1 >= arguments.Count) {
        return false;
      }

      targetIndex = operationIndex + 1;
      target = (arguments[targetIndex] ?? string.Empty).Trim().Trim('"');
      return target.Length != 0 &&
        !LooksLikeSwitch(target) &&
        !IsOperationSwitch(target);
    }

    private static bool LooksLikeAdditionalMsiTarget(string value) {
      var token = (value ?? string.Empty).Trim().Trim('"');
      return token.EndsWith(".msi", StringComparison.OrdinalIgnoreCase)
        || token.EndsWith(".msp", StringComparison.OrdinalIgnoreCase)
        || token.EndsWith(".mst", StringComparison.OrdinalIgnoreCase)
        || LooksLikeProductCode(NormalizeProductCode(token));
    }

    // This policy runs before cache cleanup, registry repair, service/process
    // changes, or self-elevation. The wrapper never elevates a caller-selected
    // package, patch, or transform. An already elevated developer may supply a
    // replacement MSI, but it is copied from a pinned handle and must match the
    // embedded native trust-boundary Binary and exact MSI table contract.
    private static bool RejectUntrustedCliPayloadRequest(
      InstallerArguments arguments,
      bool processElevated,
      out string failureMessage) {
      failureMessage = string.Empty;
      if (arguments == null) {
        failureMessage = "Installer arguments were not available.";
        return true;
      }
      var forwarded = arguments.ForwardedArguments ?? new List<string>();
      var hasOverride = !string.IsNullOrWhiteSpace(arguments.MsiPathOverride);
      if (hasOverride && !processElevated) {
        failureMessage =
          "A developer MSI may be supplied only to an already elevated installer process.";
        return true;
      }

      for (var index = 0; index < forwarded.Count; index++) {
        var raw = (forwarded[index] ?? string.Empty).Trim();
        var token = raw.Trim('"');
        var equalsIndex = token.IndexOf('=');
        var propertyName = equalsIndex > 0 ? token.Substring(0, equalsIndex) : token;
        if (string.Equals(token, "/p", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(token, "/update", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(token, "/t", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(token, "/transform", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "PATCH", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "TRANSFORMS", StringComparison.OrdinalIgnoreCase) ||
            token.EndsWith(".msp", StringComparison.OrdinalIgnoreCase) ||
            token.EndsWith(".mst", StringComparison.OrdinalIgnoreCase)) {
          failureMessage =
            "MSI patches and transforms are not accepted by the privileged bootstrapper.";
          return true;
        }
      }

      var operationCount = forwarded.Count(item =>
        IsOperationSwitch((item ?? string.Empty).Trim().Trim('"')));
      if (operationCount == 0) {
        if (forwarded.Any(item => {
              var token = (item ?? string.Empty).Trim().Trim('"');
              return token.EndsWith(".msi", StringComparison.OrdinalIgnoreCase);
            })) {
          failureMessage = "An explicit MSI requires /i or /package developer mode.";
          return true;
        }
        return false;
      }

      int operationIndex;
      int targetIndex;
      string operation;
      string target;
      if (operationCount != 1 || !TryGetSoleMsiOperationTarget(
            forwarded,
            out operationIndex,
            out operation,
            out targetIndex,
            out target)) {
        failureMessage =
          "Exactly one MSI operation and one corresponding target are required.";
        return true;
      }

      for (var index = 0; index < forwarded.Count; index++) {
        if (index == operationIndex || index == targetIndex) {
          continue;
        }
        var token = (forwarded[index] ?? string.Empty).Trim().Trim('"');
        if (IsOperationSwitch(token) || LooksLikeAdditionalMsiTarget(token)) {
          failureMessage =
            "A second MSI operation, ProductCode, or package target is not accepted.";
          return true;
        }
      }

      if (string.Equals(operation, "/uninstall", StringComparison.OrdinalIgnoreCase)) {
        failureMessage = "Use /x with a registered ProductCode for uninstall.";
        return true;
      }
      if (string.Equals(operation, "/a", StringComparison.OrdinalIgnoreCase)) {
        failureMessage = "Administrative-image MSI operations are not supported.";
        return true;
      }
      if (hasOverride) {
        failureMessage = "Do not combine --msi with a second MSI operation target.";
        return true;
      }
      if (string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase)) {
        if (hasOverride || !LooksLikeProductCode(NormalizeProductCode(target))) {
          failureMessage =
            "Uninstall accepts only a registered ProductCode; package paths are not elevated.";
          return true;
        }
        return false;
      }
      if (string.Equals(operation, "/f", StringComparison.OrdinalIgnoreCase)) {
        if (!LooksLikeProductCode(NormalizeProductCode(target))) {
          failureMessage = "Repair accepts only a registered ProductCode.";
          return true;
        }
        return false;
      }
      if ((string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) ||
           string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase)) &&
          !processElevated) {
        failureMessage =
          "An explicit MSI may be used only by an already elevated developer process.";
        return true;
      }
      return false;
    }

    private static bool TryNormalizeAndValidateMsiOperationTarget(
      List<string> cliArgs,
      out string failureMessage) {
      failureMessage = string.Empty;
      if (cliArgs == null) {
        failureMessage = "The MSI operation arguments were not available.";
        return false;
      }

      var operationIndex = cliArgs.FindIndex(IsOperationSwitch);
      if (operationIndex < 0) {
        return true;
      }
      var operation = cliArgs[operationIndex];
      var isInstall = string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
        || string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase);
      var isUninstall = string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase);
      if (!isInstall && !isUninstall) {
        return true;
      }
      if (operationIndex + 1 >= cliArgs.Count || LooksLikeSwitch(cliArgs[operationIndex + 1])) {
        failureMessage = "The MSI operation did not include a package or ProductCode target.";
        return false;
      }

      var candidate = (cliArgs[operationIndex + 1] ?? string.Empty).Trim().Trim('"');
      if (isUninstall && LooksLikeProductCode(NormalizeProductCode(candidate))) {
        return true;
      }

      string fullPath;
      try {
        fullPath = Path.GetFullPath(candidate);
      } catch (Exception ex) {
        failureMessage = "The MSI package path was invalid: " + ex.Message;
        return false;
      }
      if (!File.Exists(fullPath)) {
        failureMessage = "The MSI package was not found: " + fullPath;
        return false;
      }
      var payloadInfo = TryGetPayloadMsiInfo(fullPath);
      if (payloadInfo == null || !LooksLikeProductCode(payloadInfo.ProductCode)) {
        failureMessage = "The MSI package could not be opened or did not contain a valid ProductCode: " + fullPath;
        return false;
      }

      cliArgs[operationIndex + 1] = fullPath;
      return true;
    }

    private static InstalledProductInfo TryResolveCliMsiUninstallRecoveryProduct(List<string> cliArgs) {
      int targetIndex;
      string productCode;
      if (!TryResolveMsiUninstallTarget(cliArgs, out targetIndex, out productCode)) {
        return null;
      }

      var candidates = GetInstalledProducts(false)
        .Where(product =>
          product.IsWindowsInstaller
          && !product.IsPerUser
          && IsRecoveryKindAllowed(product.Kind, MsiRegistrationRecoveryKinds)
          && string.Equals(
            NormalizeProductCode(product.ProductCode),
            productCode,
            StringComparison.OrdinalIgnoreCase)
          && IsInstalledProductCode(product.ProductCode))
        .ToList();
      return candidates.Count == 1 ? candidates[0] : null;
    }

    private static bool CliInstallUsesSunshineVirtualDisplayDriver(List<string> cliArgs) {
      bool useSunshineDriver;
      return TryReadCliSunshineVirtualDisplayDriverSelection(cliArgs, out useSunshineDriver) && useSunshineDriver;
    }

    private static bool TryReadCliSunshineVirtualDisplayDriverSelection(List<string> cliArgs, out bool useSunshineDriver) {
      foreach (var installDirectory in ResolveCliInstallDirectoryCandidates(cliArgs)) {
        if (TryReadSunshineVirtualDisplayDriverEnabledInConfiguration(installDirectory, out useSunshineDriver)) {
          return true;
        }
      }

      useSunshineDriver = false;
      return false;
    }

    private static IEnumerable<string> ResolveCliInstallDirectoryCandidates(List<string> cliArgs) {
      var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      var installRoot = GetPropertyValue(cliArgs, "INSTALL_ROOT");
      if (!string.IsNullOrWhiteSpace(installRoot)) {
        var normalizedInstallRoot = NormalizeDirectoryPath(installRoot);
        if (seen.Add(normalizedInstallRoot)) {
          yield return normalizedInstallRoot;
        }
      }

      foreach (var installedProduct in new[] {
        GetInstalledVibepolloProduct(),
        GetInstalledVibeshineProduct(),
        GetInstalledSunshineProduct(),
        GetInstalledApolloProduct()
      }) {
        if (installedProduct == null || string.IsNullOrWhiteSpace(installedProduct.InstallLocation)) {
          continue;
        }

        var normalizedInstallLocation = NormalizeDirectoryPath(installedProduct.InstallLocation);
        if (seen.Add(normalizedInstallLocation)) {
          yield return normalizedInstallLocation;
        }
      }

      var defaultInstallDirectory = DefaultInstallDirectory;
      if (seen.Add(defaultInstallDirectory)) {
        yield return defaultInstallDirectory;
      }
    }

    private static bool ShouldElevateCliForEmbeddedPayload(IReadOnlyList<string> cliArgs, bool hasOperation) {
      if (!hasOperation) {
        return true;
      }
      if (cliArgs == null) {
        return false;
      }

      for (var index = 0; index < cliArgs.Count; index++) {
        var operation = cliArgs[index];
        if (!IsOperationSwitch(operation)) {
          continue;
        }
        if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(operation, "/a", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase)) {
          return false;
        }

        return index + 1 >= cliArgs.Count || LooksLikeSwitch(cliArgs[index + 1]);
      }

      return false;
    }

    private static string BuildCompetingProductUninstallFailureMessage(string uninstallMessage) {
      var prefix = "Failed to uninstall Apollo, Vibepollo, or Sunshine before starting Vibepollo installation.";
      if (string.IsNullOrWhiteSpace(uninstallMessage)) {
        return prefix;
      }
      return prefix + " " + uninstallMessage;
    }

    private static string BuildUpgradeSourcePreUninstallFailureMessage(string uninstallMessage) {
      var prefix = "Failed to uninstall Vibepollo 1.14.8 before starting installation."
        + " This version requires uninstall/reinstall to avoid web UI files being removed during upgrade.";
      if (string.IsNullOrWhiteSpace(uninstallMessage)) {
        return prefix;
      }
      return prefix + " " + uninstallMessage;
    }

    private static string BuildDowngradeSourcePreUninstallFailureMessage(string uninstallMessage) {
      var prefix = "Failed to uninstall the existing Vibeshine version before starting this replacement."
        + " Downgrades and same-version replacements require uninstall/reinstall because MSI cannot safely order them from the numeric product version alone.";
      if (string.IsNullOrWhiteSpace(uninstallMessage)) {
        return prefix;
      }
      return prefix + " " + uninstallMessage;
    }

    private static bool ShouldRerunCliElevatedForMsiRecovery(
      InstallerResult failureResult,
      IReadOnlyCollection<InstalledProductKind> allowedKinds) {
      if (IsProcessElevated()) {
        return false;
      }

      if (failureResult != null
          && IsRecoverableMsiFirewallCleanupFailure(failureResult.ExitCode, failureResult.LogPath)) {
        return HasConcreteMsiRegistrationRecoveryProduct(failureResult, allowedKinds);
      }
      if (!ShouldRepairBustedMsiRegistration(failureResult, allowedKinds)) {
        return false;
      }
      return BuildMsiRegistrationRecoveryTargets(failureResult, allowedKinds).Count > 0;
    }

    private static string TryGetProductLocalPackagePath(string productCode) {
      var normalized = NormalizeProductCode(productCode);
      if (!LooksLikeProductCode(normalized)) {
        return null;
      }

      uint length = 1024;
      var buffer = new StringBuilder((int)length);
      var getCode = MsiGetProductInfo(normalized, "LocalPackage", buffer, ref length);
      if (getCode == MsiErrorMoreData) {
        length += 1;
        buffer = new StringBuilder((int)length);
        getCode = MsiGetProductInfo(normalized, "LocalPackage", buffer, ref length);
      }
      if (getCode != MsiErrorSuccess) {
        return null;
      }

      var localPackage = buffer.ToString();
      return string.IsNullOrWhiteSpace(localPackage) ? null : localPackage;
    }

    private static bool MsiQueryHasRow(IntPtr database, string query) {
      IntPtr view = IntPtr.Zero;
      IntPtr record = IntPtr.Zero;
      try {
        return MsiDatabaseOpenView(database, query, out view) == MsiErrorSuccess
          && MsiViewExecute(view, IntPtr.Zero) == MsiErrorSuccess
          && MsiViewFetch(view, out record) == MsiErrorSuccess;
      } finally {
        if (record != IntPtr.Zero) {
          MsiCloseHandle(record);
        }
        if (view != IntPtr.Zero) {
          MsiViewClose(view);
          MsiCloseHandle(view);
        }
      }
    }

    private static int? TryReadMsiSequence(IntPtr database, string action) {
      IntPtr view = IntPtr.Zero;
      IntPtr record = IntPtr.Zero;
      try {
        var query = "SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = '"
          + action + "'";
        if (MsiDatabaseOpenView(database, query, out view) != MsiErrorSuccess
            || MsiViewExecute(view, IntPtr.Zero) != MsiErrorSuccess
            || MsiViewFetch(view, out record) != MsiErrorSuccess) {
          return null;
        }
        var value = MsiRecordGetInteger(record, 1);
        return value == MsiNullInteger ? (int?)null : value;
      } finally {
        if (record != IntPtr.Zero) {
          MsiCloseHandle(record);
        }
        if (view != IntPtr.Zero) {
          MsiViewClose(view);
          MsiCloseHandle(view);
        }
      }
    }

    private static string ComputeMsiBinaryStreamSha256Hex(string msiPath) {
      IntPtr database = IntPtr.Zero;
      IntPtr view = IntPtr.Zero;
      IntPtr record = IntPtr.Zero;
      try {
        if (MsiOpenDatabase(msiPath, IntPtr.Zero, out database) != MsiErrorSuccess ||
            database == IntPtr.Zero ||
            MsiDatabaseOpenView(
              database,
              "SELECT `Data` FROM `Binary` WHERE `Name`='AudioPolicyInstallTreeSecurityCA'",
              out view) != MsiErrorSuccess ||
            MsiViewExecute(view, IntPtr.Zero) != MsiErrorSuccess ||
            MsiViewFetch(view, out record) != MsiErrorSuccess) {
          return null;
        }
        var size = MsiRecordDataSize(record, 1);
        if (size == 0 || size > 64u * 1024u * 1024u) {
          return null;
        }
        var bytes = new byte[(int)size];
        uint offset = 0;
        while (offset < size) {
          var requested = Math.Min(64u * 1024u, size - offset);
          var buffer = new byte[(int)requested];
          var returned = requested;
          if (MsiRecordReadStream(record, 1, buffer, ref returned) != MsiErrorSuccess ||
              returned == 0 || returned > requested) {
            return null;
          }
          Buffer.BlockCopy(buffer, 0, bytes, (int)offset, (int)returned);
          offset += returned;
        }
        using (var hasher = SHA256.Create()) {
          return string.Concat(hasher.ComputeHash(bytes).Select(value => value.ToString("x2")));
        }
      } catch {
        return null;
      } finally {
        if (record != IntPtr.Zero) {
          MsiCloseHandle(record);
        }
        if (view != IntPtr.Zero) {
          MsiViewClose(view);
          MsiCloseHandle(view);
        }
        if (database != IntPtr.Zero) {
          MsiCloseHandle(database);
        }
      }
    }

    private static bool IsAuthenticodeTrustValid(string path) {
      IntPtr filePath = IntPtr.Zero;
      IntPtr fileInfoPointer = IntPtr.Zero;
      IntPtr trustDataPointer = IntPtr.Zero;
      try {
        filePath = Marshal.StringToCoTaskMemUni(path);
        var fileInfo = new WinTrustFileInfo {
          StructSize = (uint)Marshal.SizeOf(typeof(WinTrustFileInfo)),
          FilePath = filePath,
          FileHandle = IntPtr.Zero,
          KnownSubject = IntPtr.Zero
        };
        fileInfoPointer = Marshal.AllocCoTaskMem(Marshal.SizeOf(typeof(WinTrustFileInfo)));
        Marshal.StructureToPtr(fileInfo, fileInfoPointer, false);
        var trustData = new WinTrustData {
          StructSize = (uint)Marshal.SizeOf(typeof(WinTrustData)),
          UiChoice = WinTrustDataUiNone,
          RevocationChecks = WinTrustDataRevokeWholeChain,
          UnionChoice = WinTrustDataChoiceFile,
          FileInfo = fileInfoPointer,
          StateAction = WinTrustDataStateActionIgnore,
          ProviderFlags = WinTrustDataSaferFlag
        };
        trustDataPointer = Marshal.AllocCoTaskMem(Marshal.SizeOf(typeof(WinTrustData)));
        Marshal.StructureToPtr(trustData, trustDataPointer, false);
        var action = WinTrustActionGenericVerifyV2;
        return WinVerifyTrust(IntPtr.Zero, ref action, trustDataPointer) == 0;
      } catch {
        return false;
      } finally {
        if (trustDataPointer != IntPtr.Zero) {
          Marshal.FreeCoTaskMem(trustDataPointer);
        }
        if (fileInfoPointer != IntPtr.Zero) {
          Marshal.FreeCoTaskMem(fileInfoPointer);
        }
        if (filePath != IntPtr.Zero) {
          Marshal.FreeCoTaskMem(filePath);
        }
      }
    }

    private static string TryGetAuthenticodeSignerThumbprint(string path) {
      try {
        if (!IsAuthenticodeTrustValid(path)) {
          return null;
        }
        using (var certificate = new X509Certificate2(
          X509Certificate.CreateFromSignedFile(path))) {
          return certificate.Thumbprint;
        }
      } catch {
        return null;
      }
    }

    private static bool HasMatchingReleasedSigner(string msiPath) {
      var executableSigner = TryGetAuthenticodeSignerThumbprint(
        Assembly.GetExecutingAssembly().Location);
      var msiSigner = TryGetAuthenticodeSignerThumbprint(msiPath);
      return !string.IsNullOrWhiteSpace(executableSigner) &&
        string.Equals(executableSigner, msiSigner, StringComparison.OrdinalIgnoreCase);
    }

    private static bool MsiPackageContainsNativeTrustBoundary(string msiPath) {
      return MsiPackageContainsNativeTrustBoundary(msiPath, null, true);
    }

    private static bool MsiPackageContainsNativeTrustBoundary(
      string msiPath,
      string expectedBinarySha256,
      bool requireReleasedSigner) {
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return false;
      }
      IntPtr database = IntPtr.Zero;
      try {
        var binarySha256 = ComputeMsiBinaryStreamSha256Hex(msiPath);
        if (string.IsNullOrWhiteSpace(binarySha256) ||
            (!string.IsNullOrWhiteSpace(expectedBinarySha256) &&
             !string.Equals(binarySha256, expectedBinarySha256, StringComparison.OrdinalIgnoreCase)) ||
            (requireReleasedSigner && !HasMatchingReleasedSigner(msiPath)) ||
            MsiOpenDatabase(msiPath, IntPtr.Zero, out database) != MsiErrorSuccess
            || database == IntPtr.Zero) {
          return false;
        }
        var exactActions = new[] {
          "`Action`='PrepareAudioPolicyInstallTreeSecurity' AND `Type`=1 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='PrepareAudioPolicyInstallTreeSecurity'",
          "`Action`='PrepareAudioPolicyUpgradeSecurity' AND `Type`=1 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='PrepareAudioPolicyUpgradeSecurity'",
          "`Action`='RollbackAudioPolicyInstallTreeSecurity' AND `Type`=11585 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='RollbackAudioPolicyInstallTreeSecurity'",
          "`Action`='ProtectAudioPolicyInstallTreeSecurity' AND `Type`=11265 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='ProtectAudioPolicyInstallTreeSecurity'",
          "`Action`='CommitAudioPolicyInstallTreeSecurity' AND `Type`=11841 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='CommitAudioPolicyInstallTreeSecurity'",
          "`Action`='VerifyAudioPolicyUninstallTreeSecurity' AND `Type`=11265 AND `Source`='AudioPolicyInstallTreeSecurityCA' AND `Target`='VerifyAudioPolicyUninstallTreeSecurity'"
        };
        if (!MsiQueryHasRow(
              database,
              "SELECT `Name` FROM `Binary` WHERE `Name`='AudioPolicyInstallTreeSecurityCA'")
            || !MsiQueryHasRow(
              database,
              "SELECT `Property` FROM `Property` WHERE `Property`='AudioPolicyTrustedInstallRoot' AND `Value`='0'")
            || !MsiQueryHasRow(
              database,
              "SELECT `Property` FROM `Property` WHERE `Property`='AudioPolicyProtectRequired' AND `Value`='0'")
            || !MsiQueryHasRow(
              database,
              "SELECT `Property` FROM `Property` WHERE `Property`='AudioPolicyFullRemove' AND `Value`='0'")
            || !MsiQueryHasRow(
              database,
              "SELECT `Property` FROM `Property` WHERE `Property`='AudioPolicySecurityCABinarySha256' AND `Value`='"
              + binarySha256 + "'")
            || exactActions.Any(predicate => !MsiQueryHasRow(
              database,
              "SELECT `Action` FROM `CustomAction` WHERE " + predicate))) {
          return false;
        }

        var installFiles = TryReadMsiSequence(database, "InstallFiles");
        var installInitialize = TryReadMsiSequence(database, "InstallInitialize");
        var removeFiles = TryReadMsiSequence(database, "RemoveFiles");
        var removeExistingProducts = TryReadMsiSequence(database, "RemoveExistingProducts");
        var prepare = TryReadMsiSequence(
          database,
          "PrepareAudioPolicyInstallTreeSecurity");
        var protect = TryReadMsiSequence(
          database,
          "ProtectAudioPolicyInstallTreeSecurity");
        var verify = TryReadMsiSequence(
          database,
          "VerifyAudioPolicyUninstallTreeSecurity");
        var prepareUpgrade = TryReadMsiSequence(
          database,
          "PrepareAudioPolicyUpgradeSecurity");
        var rollback = TryReadMsiSequence(
          database,
          "RollbackAudioPolicyInstallTreeSecurity");
        var commit = TryReadMsiSequence(
          database,
          "CommitAudioPolicyInstallTreeSecurity");
        var startServices = TryReadMsiSequence(database, "StartServices");
        var exactSequenceConditions = new[] {
          "`Action`='PrepareAudioPolicyInstallTreeSecurity' AND `Condition`='NOT WIX_UPGRADE_DETECTED'",
          "`Action`='PrepareAudioPolicyUpgradeSecurity' AND `Condition`='WIX_UPGRADE_DETECTED'",
          "`Action`='RollbackAudioPolicyInstallTreeSecurity' AND `Condition`='AudioPolicyProtectRequired = \"1\" AND AudioPolicyTrustedInstallRoot = \"1\"'",
          "`Action`='ProtectAudioPolicyInstallTreeSecurity' AND `Condition`='AudioPolicyProtectRequired = \"1\" AND AudioPolicyTrustedInstallRoot = \"1\"'",
          "`Action`='CommitAudioPolicyInstallTreeSecurity' AND `Condition`='AudioPolicyProtectRequired = \"1\" AND AudioPolicyTrustedInstallRoot = \"1\"'",
          "`Action`='VerifyAudioPolicyUninstallTreeSecurity' AND `Condition`='AudioPolicyFullRemove = \"1\" AND NOT UPGRADINGPRODUCTCODE AND AudioPolicyTrustedInstallRoot = \"1\"'"
        };
        return installInitialize.HasValue && installFiles.HasValue && removeFiles.HasValue &&
          removeExistingProducts.HasValue && prepare.HasValue && prepareUpgrade.HasValue &&
          rollback.HasValue && protect.HasValue && commit.HasValue && verify.HasValue &&
          startServices.HasValue &&
          exactSequenceConditions.All(predicate => MsiQueryHasRow(
            database,
            "SELECT `Action` FROM `InstallExecuteSequence` WHERE " + predicate)) &&
          prepare.Value > installInitialize.Value &&
          prepare.Value < installFiles.Value &&
          prepareUpgrade.Value > installInitialize.Value &&
          prepareUpgrade.Value < removeExistingProducts.Value &&
          removeExistingProducts.Value < installFiles.Value &&
          rollback.Value > installFiles.Value &&
          protect.Value > rollback.Value &&
          commit.Value > protect.Value &&
          protect.Value > installFiles.Value &&
          protect.Value < startServices.Value &&
          verify.Value < removeFiles.Value;
      } catch {
        return false;
      } finally {
        if (database != IntPtr.Zero) {
          MsiCloseHandle(database);
        }
      }
    }

    private static StashedVibeshinePayload TryStashInstalledProductPayload(
      InstalledProductInfo installedProduct,
      string logPhase) {
      string recoveryDirectory = null;
      try {
        if (installedProduct == null) {
          return null;
        }

        var localPackage = TryGetProductLocalPackagePath(installedProduct.ProductCode);
        if (string.IsNullOrWhiteSpace(localPackage) || !File.Exists(localPackage)) {
          return null;
        }

        string recoveryError;
        if (!TryCreateSecureInstallerRecoveryDirectory(out recoveryDirectory, out recoveryError)) {
          if (!string.IsNullOrWhiteSpace(recoveryDirectory)) {
            TryDeleteInstallerRecoveryDirectory(recoveryDirectory);
          }
          return null;
        }
        var stashPath = Path.Combine(recoveryDirectory, "previous_cached.msi");
        File.Copy(localPackage, stashPath, false);
        var stashedMsiInfo = TryGetPayloadMsiInfo(stashPath);
        if (stashedMsiInfo == null
            || !string.Equals(
              NormalizeProductCode(stashedMsiInfo.ProductCode),
              NormalizeProductCode(installedProduct.ProductCode),
              StringComparison.OrdinalIgnoreCase)) {
          TryDeleteInstallerRecoveryDirectory(recoveryDirectory);
          return null;
        }

        return new StashedVibeshinePayload {
          MsiPath = stashPath,
          ProductCode = NormalizeProductCode(installedProduct.ProductCode),
          InstallLocation = installedProduct.InstallLocation ?? string.Empty,
          RecoveryDirectory = recoveryDirectory
        };
      } catch {
        if (!string.IsNullOrWhiteSpace(recoveryDirectory)) {
          TryDeleteInstallerRecoveryDirectory(recoveryDirectory);
        }
        return null;
      }
    }

    private static InstallerResult BuildRollbackPreservationFailure(InstalledProductInfo installedProduct) {
      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = 1603,
        Message = "The installer could not preserve an exact rollback copy of "
          + BuildProductDisplayName(installedProduct)
          + " before the required uninstall. The existing installation was left unchanged.",
        ProductCode = installedProduct == null ? string.Empty : (installedProduct.ProductCode ?? string.Empty),
        ProductDisplayName = installedProduct == null ? string.Empty : (installedProduct.DisplayName ?? string.Empty),
        ProductKind = installedProduct == null ? InstalledProductKind.Unknown : installedProduct.Kind
      };
    }

    private static void TryDeleteStashedPayload(StashedVibeshinePayload stashedPayload) {
      if (stashedPayload == null) {
        return;
      }
      if (!string.IsNullOrWhiteSpace(stashedPayload.RecoveryDirectory)) {
        TryDeleteInstallerRecoveryDirectory(stashedPayload.RecoveryDirectory);
      } else {
        TryDeleteFile(stashedPayload.MsiPath);
      }
    }

    private static void AdoptStashedPayload(
      ref StashedVibeshinePayload selectedPayload,
      StashedVibeshinePayload candidatePayload) {
      if (candidatePayload == null || object.ReferenceEquals(selectedPayload, candidatePayload)) {
        return;
      }
      if (selectedPayload == null) {
        selectedPayload = candidatePayload;
        return;
      }
      TryDeleteStashedPayload(candidatePayload);
    }

    private static string TryRestoreStashedVibeshinePayload(StashedVibeshinePayload stashedPayload, string logPhase) {
      if (stashedPayload == null || string.IsNullOrWhiteSpace(stashedPayload.MsiPath) || !File.Exists(stashedPayload.MsiPath)) {
        return null;
      }

      try {
        if (GetInstalledVibepolloProduct() != null || GetInstalledVibeshineProduct() != null) {
          // A product is still (or again) registered; leave it alone.
          return null;
        }

        var logPath = BuildLogPath(logPhase);
        var args = new List<string> {
          "/i",
          stashedPayload.MsiPath,
          "/qn",
          "/norestart",
          "/l*v",
          logPath,
          "SKIP_REMOVE_CONFLICTING_PRODUCTS=1",
          "REBOOT=ReallySuppress",
          "SUPPRESSMSGBOXES=1"
        };
        if (!string.IsNullOrWhiteSpace(stashedPayload.InstallLocation)) {
          args.Add(CreatePropertyArgument("INSTALL_ROOT", stashedPayload.InstallLocation));
        }

        AppendInstallerLogMessage(logPath, "Restoring previously installed version from stashed package: " + stashedPayload.MsiPath);
        var exitCode = RunMsiexec(args, true, false);
        if ((exitCode == 0 || exitCode == 3010) && (GetInstalledVibepolloProduct() != null || GetInstalledVibeshineProduct() != null)) {
          return "The previously installed version was automatically restored. Restore log: " + logPath;
        }

        return "Automatic restore of the previously installed version failed (exit code " + exitCode
          + "). The previous package was saved to: " + stashedPayload.MsiPath
          + " and can be run manually, but Windows Installer's cached package is not a complete standalone installer, so restoring the prior version may require the original installer source. Restore log: "
          + logPath;
      } catch (Exception ex) {
        return "Automatic restore of the previously installed version failed: " + ex.Message
          + " The previous package was saved to: " + stashedPayload.MsiPath
          + " and can be run manually, but Windows Installer's cached package is not a complete standalone installer, so restoring the prior version may require the original installer source.";
      }
    }

    private static InstallerResult ApplyStashedPayloadRecovery(
      InstallerResult installResult,
      StashedVibeshinePayload stashedPayload,
      string logPhase) {
      if (installResult == null || stashedPayload == null) {
        return installResult;
      }

      if (installResult.Succeeded) {
        if (!string.IsNullOrWhiteSpace(stashedPayload.RecoveryDirectory)) {
          TryDeleteInstallerRecoveryDirectory(stashedPayload.RecoveryDirectory);
        } else {
          TryDeleteFile(stashedPayload.MsiPath);
        }
        return installResult;
      }

      var restoreMessage = TryRestoreStashedVibeshinePayload(stashedPayload, logPhase);
      if (!string.IsNullOrWhiteSpace(restoreMessage)) {
        installResult.Message = string.IsNullOrWhiteSpace(installResult.Message)
          ? restoreMessage
          : installResult.Message.TrimEnd() + " " + restoreMessage;
      }
      var restoredVibepollo = GetInstalledVibepolloProduct();
      var restoredVibeshine = GetInstalledVibeshineProduct();
      var restoredExactProduct =
        (restoredVibepollo != null
          && string.Equals(
            NormalizeProductCode(restoredVibepollo.ProductCode),
            NormalizeProductCode(stashedPayload.ProductCode),
            StringComparison.OrdinalIgnoreCase))
        || (restoredVibeshine != null
          && string.Equals(
            NormalizeProductCode(restoredVibeshine.ProductCode),
            NormalizeProductCode(stashedPayload.ProductCode),
            StringComparison.OrdinalIgnoreCase));
      if (restoredExactProduct) {
        if (!string.IsNullOrWhiteSpace(stashedPayload.RecoveryDirectory)) {
          TryDeleteInstallerRecoveryDirectory(stashedPayload.RecoveryDirectory);
        } else {
          TryDeleteFile(stashedPayload.MsiPath);
        }
      }
      return installResult;
    }

    private static InstallerResult TryPreUninstallDowngradeSourceVersion(
      string msiPath,
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      out StashedVibeshinePayload stashedPayload) {
      stashedPayload = null;
      var installedVibeshine = GetInstalledVibeshineProduct();
      return RequiresPreUninstallDowngradeWorkaround(installedVibeshine, msiPath)
        ? ManualLegacyRemovalRequired(BuildProductDisplayName(installedVibeshine))
        : null;
    }

    private static InstallerResult TryPreUninstallProblematicUpgradeSourceVersion(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      out StashedVibeshinePayload stashedPayload) {
      stashedPayload = null;
      return null;
    }


    private static bool RequiresPreUninstallDowngradeWorkaround(InstalledProductInfo installedProduct, string msiPath) {
      if (installedProduct == null || installedProduct.Kind != InstalledProductKind.Vibeshine || installedProduct.Version == null) {
        return false;
      }

      var payloadMsiInfo = TryGetPayloadMsiInfo(msiPath);
      if (payloadMsiInfo == null || payloadMsiInfo.Version == null) {
        return false;
      }

      if (PayloadSupportsTransactionalReplacement(payloadMsiInfo)) {
        // The payload authors MajorUpgrade AllowDowngrades="yes" with an
        // ordinal-encoded ProductVersion: same-version and downgrade
        // replacement runs inside the MSI transaction and rolls back to the
        // installed version if anything fails. A standalone pre-uninstall
        // would reintroduce the unprotected window where neither version is
        // installed, so it must be skipped.
        return false;
      }

      if (installedProduct.Version > payloadMsiInfo.Version) {
        return true;
      }

      return installedProduct.Version.CompareTo(payloadMsiInfo.Version) == 0
        && HasDifferentProductCode(installedProduct.ProductCode, payloadMsiInfo.ProductCode);
    }

    private static bool PayloadSupportsTransactionalReplacement(PayloadMsiInfo payloadMsiInfo) {
      // Set by VIBESHINE_TRANSACTIONAL_REPLACEMENT=1 in WIX.template.in; only
      // legacy payloads (which block downgrades and same-version installs)
      // lack it and still need the uninstall-then-install workaround.
      return payloadMsiInfo != null && payloadMsiInfo.SupportsTransactionalReplacement;
    }

    private static bool HasDifferentProductCode(string installedProductCode, string payloadProductCode) {
      var installed = NormalizeProductCode(installedProductCode);
      var payload = NormalizeProductCode(payloadProductCode);
      return LooksLikeProductCode(installed)
        && LooksLikeProductCode(payload)
        && !string.Equals(installed, payload, StringComparison.OrdinalIgnoreCase);
    }

    private static bool RequiresPreUninstallUpgradeWorkaround(InstalledProductInfo installedProduct) {
      if (installedProduct == null || installedProduct.Kind != InstalledProductKind.Vibepollo || installedProduct.Version == null) {
        return false;
      }

      if (installedProduct.Version.Major != UpgradeSourcePreUninstallVersion.Major
          || installedProduct.Version.Minor != UpgradeSourcePreUninstallVersion.Minor) {
        return false;
      }

      // The 1.14.8 registration may surface either as a raw build (8, from a
      // four-part ProductVersion string) or ordinal-encoded (600..699, when
      // ParseVersion mapped a three-part DisplayVersion).
      return installedProduct.Version.Build == UpgradeSourcePreUninstallVersion.Build
        || (installedProduct.Version.Build >= UpgradeSourcePreUninstallVersion.Build * 100
          && installedProduct.Version.Build <= UpgradeSourcePreUninstallVersion.Build * 100 + 99);
    }

    private static InstallerResult UninstallCompetingProducts(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded) {
      var conflictingProduct = GetInstalledProductRegistrations(true)
        .Where(product =>
          product.Kind == InstalledProductKind.Apollo
          || product.Kind == InstalledProductKind.Vibepollo
          || product.Kind == InstalledProductKind.Sunshine)
        .GroupBy(BuildProductRegistrationIdentity, StringComparer.OrdinalIgnoreCase)
        .Select(MergeInstalledProductGroup)
        .Where(product => product.Kind != InstalledProductKind.Vibepollo)
        .FirstOrDefault(CanUninstallProduct);
      return conflictingProduct == null
        ? new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "No conflicting Apollo or Sunshine installation was found."
        }
        : ManualLegacyRemovalRequired(BuildProductDisplayName(conflictingProduct));
    }


    private static InstallerResult UninstallInstalledProducts(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      bool factoryResetAppData,
      bool removeVirtualDisplayDriver,
      bool failWhenMissing,
      IReadOnlyCollection<InstalledProductKind> uninstallKinds) {
      var kinds = uninstallKinds ?? Array.Empty<InstalledProductKind>();
      var installedProducts = GetInstalledProducts(true)
        .Where(product => kinds.Count == 0 || kinds.Contains(product.Kind))
        .ToList();
      if (installedProducts.Count == 0) {
        var missingMessage = "No matching installations were found.";
        if (kinds.Count == 1) {
          var singleKind = kinds.First();
          missingMessage = "No existing " + singleKind + " installation was found.";
        }
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = failWhenMissing ? 1605 : 0,
          Message = missingMessage
        };
      }

      var finalCode = 0;
      var lastLogPath = string.Empty;
      foreach (var product in installedProducts) {
        if (product.IsPerUser || !product.IsWindowsInstaller) {
          return ManualLegacyRemovalRequired(
            product.DisplayName ?? "a per-user or non-MSI product");
        }
        var cachedPackage = TryGetProductLocalPackagePath(product.ProductCode);
        var releasedPayload = OpenPinnedReleasedMsiPayload(
          cachedPackage,
          product.ProductCode);
        if (releasedPayload == null) {
          return ManualLegacyRemovalRequired(
            BuildProductDisplayName(product));
        }
        using (releasedPayload) {
        var logPath = BuildLogPath(logPhase + "_remove");
        lastLogPath = logPath;

        var args = new List<string> {
          "/x",
          product.ProductCode,
          "/qn",
          "/norestart",
          "/l*v",
          logPath,
          "FACTORYRESET=" + (factoryResetAppData ? "1" : "0"),
          "REMOVEVIRTUALDISPLAYDRIVER=" + (removeVirtualDisplayDriver ? "1" : "0"),
          "REBOOT=ReallySuppress",
          "SUPPRESSMSGBOXES=1"
        };

        AppendInstallerLogMessage(logPath, "Quiescing related services and helper processes before MSI uninstall attempt.");
        TryStopRelatedServicesAndProcesses(logPath);
        CleanupStaleComponentClientsForInstallLocation(product.InstallLocation, logPath);

        var code = RunMsiexec(args, hiddenWindow, requestElevationIfNeeded, releasedPayload);
        if (IsRecoverableMsiFirewallCleanupFailure(code, logPath)) {
          int retryCode;
          string retryLogPath;
          string recoveryDetail;
          StashedVibeshinePayload ignoredStashedPayload;
          if (TryRunFirewallTolerantUninstall(
            product,
            args,
            logPath,
            logPhase + "_remove_firewall_cleanup_recovery",
            hiddenWindow,
            requestElevationIfNeeded,
            false,
            out retryCode,
            out retryLogPath,
            out recoveryDetail,
            out ignoredStashedPayload)) {
            code = retryCode;
            logPath = retryLogPath;
            lastLogPath = retryLogPath;
          }
        }
        if (code == 0 || code == 3010 || code == 1605) {
          CleanupCustomArpRegistration(product.InstallLocation, logPath);
          ScheduleSelfDeleteAndEmptyInstallRootCleanup(product.InstallLocation, logPath);
        }
        if (factoryResetAppData && (code == 0 || code == 3010 || code == 1605)) {
          TryFactoryResetKnownAppData(product.InstallLocation);
        }
        if (code == 3010) {
          finalCode = 3010;
          continue;
        }
        if (code == 0 || code == 1605) {
          continue;
        }

        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = code,
          Message = BuildResultMessage("Uninstall", code, logPath),
          LogPath = logPath,
          ProductCode = product.ProductCode ?? string.Empty,
          ProductDisplayName = product.DisplayName ?? string.Empty,
          ProductKind = product.Kind
        };
      }
      }

      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = finalCode,
        Message = BuildResultMessage("Uninstall", finalCode, lastLogPath),
        LogPath = lastLogPath
      };
    }

    private static void CleanupCustomArpRegistration(string installLocation, string logPath) {
      var normalizedInstallLocation = NormalizePath(installLocation);
      if (string.IsNullOrWhiteSpace(normalizedInstallLocation)) {
        return;
      }

      foreach (var hive in GetRegistryCleanupHives()) {
        foreach (var view in GetRegistryCleanupViews()) {
          try {
            using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
            using (var uninstallRoot = baseKey.OpenSubKey(
              @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
              true)) {
              if (uninstallRoot == null || !uninstallRoot.GetSubKeyNames().Contains("Vibeshine", StringComparer.OrdinalIgnoreCase)) {
                continue;
              }

              using (var vibeshineKey = uninstallRoot.OpenSubKey("Vibeshine", false)) {
                if (vibeshineKey == null) {
                  continue;
                }

                var displayName = Convert.ToString(vibeshineKey.GetValue("DisplayName")) ?? string.Empty;
                var keyInstallLocation = Convert.ToString(vibeshineKey.GetValue("InstallLocation")) ?? string.Empty;
                var uninstallString = Convert.ToString(vibeshineKey.GetValue("UninstallString")) ?? string.Empty;
                var quietUninstallString = Convert.ToString(vibeshineKey.GetValue("QuietUninstallString")) ?? string.Empty;
                var matchesInstallLocation =
                  PathIsUnderDirectory(keyInstallLocation, normalizedInstallLocation)
                  || CommandReferencesInstallLocation(uninstallString, normalizedInstallLocation)
                  || CommandReferencesInstallLocation(quietUninstallString, normalizedInstallLocation);
                if (!displayName.StartsWith("Vibeshine", StringComparison.OrdinalIgnoreCase) || !matchesInstallLocation) {
                  continue;
                }
              }

              uninstallRoot.DeleteSubKeyTree("Vibeshine", false);
              AppendInstallerLogMessage(
                logPath,
                "Removed orphan Vibeshine ARP uninstall entry from " + hive
                + @"\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Vibeshine (" + view + ").");
            }
          } catch (Exception ex) {
            AppendInstallerLogMessage(
              logPath,
              "Unable to remove orphan Vibeshine ARP uninstall entry in " + hive + " " + view + ": " + ex.Message);
          }
        }
      }
    }

    private static bool CommandReferencesInstallLocation(string commandLine, string installLocation) {
      string executablePath;
      string arguments;
      if (!TrySplitExecutableAndArguments(commandLine, out executablePath, out arguments)) {
        return false;
      }
      return PathIsUnderDirectory(executablePath, installLocation);
    }

    private static void ScheduleSelfDeleteAndEmptyInstallRootCleanup(string installLocation, string logPath) {
      var normalizedInstallLocation = NormalizePath(installLocation);
      var executablePath = NormalizePath(Assembly.GetExecutingAssembly().Location);
      if (string.IsNullOrWhiteSpace(normalizedInstallLocation)
          || string.IsNullOrWhiteSpace(executablePath)
          || !PathIsUnderDirectory(executablePath, normalizedInstallLocation)
          || !string.Equals(Path.GetFileName(executablePath), "uninstall.exe", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      try {
        var cmdPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");
        var command = "ping 127.0.0.1 -n 3 > nul"
          + " & del /F /Q " + QuoteForCmd(executablePath) + " > nul 2> nul"
          + " & rd " + QuoteForCmd(normalizedInstallLocation) + " 2> nul";
        Process.Start(new ProcessStartInfo {
          FileName = cmdPath,
          Arguments = "/D /C " + command,
          UseShellExecute = false,
          CreateNoWindow = true,
          WindowStyle = ProcessWindowStyle.Hidden
        });
        AppendInstallerLogMessage(logPath, "Scheduled uninstall.exe self-delete and empty install root cleanup.");
      } catch (Exception ex) {
        AppendInstallerLogMessage(logPath, "Unable to schedule uninstall.exe self-delete: " + ex.Message);
      }
    }

    private static string QuoteForCmd(string value) {
      return "\"" + (value ?? string.Empty).Replace("\"", string.Empty) + "\"";
    }

    private static void TryFactoryResetKnownAppData(string installLocation) {
      if (string.IsNullOrWhiteSpace(installLocation)) {
        return;
      }

      try {
        var root = Path.GetFullPath(installLocation.Trim().Trim('"'));
        if (!IsSafeInstallRootForFactoryReset(root)) {
          return;
        }

        var knownItems = new[] {
          "apps.json",
          "sunshine.conf",
          "sunshine.log",
          "sunshine_state.json",
          "vibeshine_state.json",
          "virtual_display_cache.json",
          "nvprefs_undo.json",
          "sunshine_playnite.log",
          "credentials",
          "session_history",
          "covers",
          "logs"
        };

        var config = Path.Combine(root, "config");
        foreach (var item in knownItems) {
          TryDeleteKnownPath(Path.Combine(config, item));
        }
        foreach (var item in knownItems) {
          TryDeleteKnownPath(Path.Combine(root, item));
        }
        TryDeleteDirectoryIfEmpty(config);
      } catch {
        // Factory reset cleanup is best-effort. MSI-owned files are still
        // removed by Windows Installer, and user-added files must be preserved.
      }
    }

    private static bool IsSafeInstallRootForFactoryReset(string root) {
      if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root)) {
        return false;
      }

      var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
      var pathRoot = Path.GetPathRoot(fullRoot);
      if (string.IsNullOrWhiteSpace(fullRoot)
          || string.Equals(
            fullRoot,
            (pathRoot ?? string.Empty).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
            StringComparison.OrdinalIgnoreCase)) {
        return false;
      }

      var sentinelFiles = new[] {
        Path.Combine(fullRoot, "sunshine.exe"),
        Path.Combine(fullRoot, "uninstall.exe"),
        Path.Combine(fullRoot, "scripts", "factory-reset-appdata.ps1")
      };
      return sentinelFiles.Any(File.Exists);
    }

    private static void TryDeleteKnownPath(string path) {
      try {
        if (string.IsNullOrWhiteSpace(path)) {
          return;
        }
        if (Directory.Exists(path)) {
          Directory.Delete(path, true);
        } else if (File.Exists(path)) {
          File.Delete(path);
        }
      } catch {
      }
    }

    private static void TryDeleteDirectoryIfEmpty(string path) {
      try {
        if (string.IsNullOrWhiteSpace(path) || !Directory.Exists(path)) {
          return;
        }
        if (!Directory.EnumerateFileSystemEntries(path).Any()) {
          Directory.Delete(path, false);
        }
      } catch {
      }
    }

    private static string BuildProductDisplayName(InstalledProductInfo product) {
      if (product == null || string.IsNullOrWhiteSpace(product.DisplayName)) {
        return "the conflicting product";
      }
      return product.DisplayName.Trim();
    }

    private static string BuildProductUninstallFailureMessage(InstalledProductInfo product, int exitCode, string logPath) {
      var message = "Failed to uninstall " + BuildProductDisplayName(product) + ".";
      if (!string.IsNullOrWhiteSpace(logPath)) {
        message += " MSI log: " + logPath;
      } else {
        message += " Exit code: " + exitCode + ".";
      }
      return message;
    }

    private static bool IsTrustedInstallerCacheIdentity(IdentityReference identity) {
      var sid = identity as SecurityIdentifier;
      if (sid == null) {
        try {
          sid = (SecurityIdentifier)identity.Translate(typeof(SecurityIdentifier));
        } catch {
          return false;
        }
      }
      return sid.IsWellKnown(WellKnownSidType.LocalSystemSid)
        || sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid)
        || string.Equals(
          sid.Value,
          "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
          StringComparison.Ordinal);
    }

    private static bool InstallerCacheSecurityIsSafe(
      FileSystemSecurity security,
      bool strictMutationCheck,
      bool requireProtected = true) {
      if (security == null || (requireProtected && !security.AreAccessRulesProtected) ||
          !IsTrustedInstallerCacheIdentity(security.GetOwner(typeof(SecurityIdentifier)))) {
        return false;
      }
      const FileSystemRights namespaceDanger =
        FileSystemRights.Delete |
        FileSystemRights.DeleteSubdirectoriesAndFiles |
        FileSystemRights.ChangePermissions |
        FileSystemRights.TakeOwnership;
      const FileSystemRights mutationDanger =
        namespaceDanger |
        FileSystemRights.CreateFiles |
        FileSystemRights.CreateDirectories |
        FileSystemRights.Write |
        FileSystemRights.WriteAttributes |
        FileSystemRights.WriteExtendedAttributes |
        FileSystemRights.Modify |
        FileSystemRights.FullControl;
      var danger = strictMutationCheck ? mutationDanger : namespaceDanger;
      foreach (FileSystemAccessRule rule in security.GetAccessRules(
        true,
        true,
        typeof(SecurityIdentifier))) {
        if (rule.AccessControlType != AccessControlType.Allow ||
            IsTrustedInstallerCacheIdentity(rule.IdentityReference)) {
          continue;
        }
        if ((rule.PropagationFlags & PropagationFlags.InheritOnly) != 0 &&
            !strictMutationCheck) {
          continue;
        }
        if ((rule.FileSystemRights & danger) != 0) {
          return false;
        }
      }
      return true;
    }

    private static string NormalizeFinalHandlePath(string path) {
      var normalized = path ?? string.Empty;
      if (normalized.StartsWith(@"\\?\", StringComparison.Ordinal)) {
        normalized = normalized.Substring(4);
      }
      return Path.GetFullPath(normalized)
        .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    private static bool HandlePathMatches(SafeFileHandle handle, string expectedPath) {
      if (handle == null || handle.IsInvalid || string.IsNullOrWhiteSpace(expectedPath)) {
        return false;
      }
      var capacity = 512u;
      for (var attempt = 0; attempt < 3; attempt++) {
        var buffer = new StringBuilder((int)capacity);
        var written = GetFinalPathNameByHandle(
          handle,
          buffer,
          capacity,
          FileNameNormalized | VolumeNameDos);
        if (written == 0) {
          return false;
        }
        if (written < capacity) {
          return string.Equals(
            NormalizeFinalHandlePath(buffer.ToString()),
            Path.GetFullPath(expectedPath).TrimEnd(
              Path.DirectorySeparatorChar,
              Path.AltDirectorySeparatorChar),
            StringComparison.OrdinalIgnoreCase);
        }
        capacity = written + 1;
      }
      return false;
    }

    private static SafeFileHandle OpenPinnedInstallerDirectory(string path) {
      var handle = CreateFile(
        path,
        ReadControl | FileReadAttributes,
        FileShareRead | FileShareWrite,
        IntPtr.Zero,
        OpenExisting,
        FileFlagBackupSemantics | FileFlagOpenReparsePoint,
        IntPtr.Zero);
      if (handle == null || handle.IsInvalid) {
        if (handle != null) {
          handle.Dispose();
        }
        return null;
      }
      ByHandleFileInformation information;
      if (!GetFileInformationByHandle(handle, out information) ||
          (information.FileAttributes & FileAttributeReparsePoint) != 0 ||
          !HandlePathMatches(handle, path)) {
        handle.Dispose();
        return null;
      }
      return handle;
    }

    private static List<SafeFileHandle> PinAndValidateInstallerCacheAncestors(
      string leafDirectory) {
      var windows = Path.GetFullPath(
        Environment.GetFolderPath(Environment.SpecialFolder.Windows)).TrimEnd(
          Path.DirectorySeparatorChar,
          Path.AltDirectorySeparatorChar);
      return PinAndValidateTrustedWindowsAncestors(
        leafDirectory,
        Path.Combine(windows, "SystemTemp"));
    }

    private static List<SafeFileHandle> PinAndValidateTrustedWindowsAncestors(
      string leafDirectory,
      string protectedRoot) {
      var fullLeaf = Path.GetFullPath(leafDirectory).TrimEnd(
        Path.DirectorySeparatorChar,
        Path.AltDirectorySeparatorChar);
      var windows = Path.GetFullPath(
        Environment.GetFolderPath(Environment.SpecialFolder.Windows)).TrimEnd(
          Path.DirectorySeparatorChar,
          Path.AltDirectorySeparatorChar);
      var fullProtectedRoot = Path.GetFullPath(protectedRoot).TrimEnd(
        Path.DirectorySeparatorChar,
        Path.AltDirectorySeparatorChar);
      var windowsPrefix = windows + Path.DirectorySeparatorChar;
      var requiredPrefix = fullProtectedRoot + Path.DirectorySeparatorChar;
      if ((!string.Equals(fullProtectedRoot, windows, StringComparison.OrdinalIgnoreCase) &&
           !fullProtectedRoot.StartsWith(windowsPrefix, StringComparison.OrdinalIgnoreCase)) ||
          (!string.Equals(fullLeaf, fullProtectedRoot, StringComparison.OrdinalIgnoreCase) &&
           !fullLeaf.StartsWith(requiredPrefix, StringComparison.OrdinalIgnoreCase))) {
        return null;
      }
      var paths = new List<string>();
      var cursor = fullLeaf;
      var volumeRoot = Path.GetPathRoot(windows).TrimEnd(
        Path.DirectorySeparatorChar,
        Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
      for (;;) {
        paths.Add(cursor);
        if (string.Equals(
              cursor.TrimEnd(Path.DirectorySeparatorChar),
              volumeRoot.TrimEnd(Path.DirectorySeparatorChar),
              StringComparison.OrdinalIgnoreCase)) {
          break;
        }
        var parent = Directory.GetParent(cursor);
        if (parent == null) {
          return null;
        }
        cursor = parent.FullName;
      }
      paths.Reverse();
      var pins = new List<SafeFileHandle>();
      try {
        foreach (var path in paths) {
          if (!Directory.Exists(path)) {
            throw new InvalidOperationException("A trusted installer-cache ancestor is missing.");
          }
          var pin = OpenPinnedInstallerDirectory(path);
          if (pin == null) {
            throw new InvalidOperationException("A trusted installer-cache ancestor could not be pinned.");
          }
          var strict = string.Equals(path, fullProtectedRoot, StringComparison.OrdinalIgnoreCase)
            || path.StartsWith(requiredPrefix, StringComparison.OrdinalIgnoreCase);
          var security = new DirectoryInfo(path).GetAccessControl(
            AccessControlSections.Access | AccessControlSections.Owner);
          if (!InstallerCacheSecurityIsSafe(security, strict)) {
            pin.Dispose();
            throw new InvalidOperationException("A trusted installer-cache ancestor has unsafe access rules.");
          }
          pins.Add(pin);
        }
        return pins;
      } catch {
        foreach (var pin in pins) {
          pin.Dispose();
        }
        return null;
      }
    }

    private static DirectorySecurity BuildInstallerCacheDirectorySecurity() {
      return BuildInstallerRecoveryDirectorySecurity();
    }

    private static FileSecurity BuildInstallerCacheFileSecurity() {
      var security = new FileSecurity();
      security.SetAccessRuleProtection(true, false);
      var administratorsSid = new SecurityIdentifier(
        WellKnownSidType.BuiltinAdministratorsSid,
        null);
      security.SetOwner(administratorsSid);
      security.AddAccessRule(new FileSystemAccessRule(
        new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
        FileSystemRights.FullControl,
        AccessControlType.Allow));
      security.AddAccessRule(new FileSystemAccessRule(
        administratorsSid,
        FileSystemRights.FullControl,
        AccessControlType.Allow));
      return security;
    }

    private static string CreateTrustedInstallerCacheRoot() {
      var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
      if (string.IsNullOrWhiteSpace(windows)) {
        throw new InvalidOperationException("The Windows directory could not be resolved.");
      }
      var systemTemp = Path.Combine(Path.GetFullPath(windows), "SystemTemp");
      var systemPins = PinAndValidateInstallerCacheAncestors(systemTemp);
      if (systemPins == null) {
        throw new InvalidOperationException("The LocalSystem temporary directory is not trusted.");
      }
      foreach (var pin in systemPins) {
        pin.Dispose();
      }

      // Never reuse or repair a predictable cache directory. A pre-existing
      // path could be a junction or carry an attacker-selected descriptor.
      // A fresh, non-reparse leaf under the already pinned SystemTemp parent
      // is the per-process trust root for every payload passed to msiexec.
      for (var attempt = 0; attempt < 8; attempt++) {
        var root = Path.Combine(
          systemTemp,
          "VibepolloInstallerCache_" + Guid.NewGuid().ToString("N"));
        if (Directory.Exists(root) || File.Exists(root)) {
          continue;
        }
        var rootInfo = new DirectoryInfo(root);
        rootInfo.Create(BuildInstallerCacheDirectorySecurity());
        var pins = PinAndValidateInstallerCacheAncestors(root);
        if (pins == null) {
          throw new InvalidOperationException("The fresh installer-cache root is not trusted.");
        }
        foreach (var pin in pins) {
          pin.Dispose();
        }
        return root;
      }
      throw new InvalidOperationException("A fresh installer-cache root could not be created.");
    }

    private static string GetTrustedInstallerCacheRoot() {
      return TrustedInstallerCacheRoot.Value;
    }

    private static void CreateProtectedInstallerCacheDirectory(string path) {
      if (!IsProcessElevated()) {
        Directory.CreateDirectory(path);
        return;
      }
      var root = GetTrustedInstallerCacheRoot();
      var fullPath = Path.GetFullPath(path);
      if (!string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase) &&
          !fullPath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) {
        throw new InvalidOperationException("The MSI cache path escaped its trusted root.");
      }
      var info = new DirectoryInfo(fullPath);
      if (!info.Exists) {
        info.Create(BuildInstallerCacheDirectorySecurity());
      }
      info.SetAccessControl(BuildInstallerCacheDirectorySecurity());
      var pins = PinAndValidateInstallerCacheAncestors(fullPath);
      if (pins == null) {
        throw new InvalidOperationException("The MSI cache directory is not trusted.");
      }
      foreach (var pin in pins) {
        pin.Dispose();
      }
    }

    private static void SetProtectedInstallerCacheFileSecurity(string path) {
      if (IsProcessElevated()) {
        new FileInfo(path).SetAccessControl(BuildInstallerCacheFileSecurity());
      }
    }

    private static string ComputePinnedStreamSha256Hex(FileStream stream) {
      if (stream == null || !stream.CanSeek) {
        throw new InvalidOperationException("The MSI payload stream cannot be hashed.");
      }
      var original = stream.Position;
      try {
        stream.Position = 0;
        using (var hasher = SHA256.Create()) {
          return string.Concat(hasher.ComputeHash(stream).Select(b => b.ToString("x2")));
        }
      } finally {
        stream.Position = original;
      }
    }

    private static void RegisterApprovedPayload(string path, string sha256) {
      var fullPath = Path.GetFullPath(path);
      lock (ApprovedPayloadLock) {
        ApprovedPayloadHashes[fullPath] = sha256;
      }
    }

    private static PinnedMsiPayload OpenPinnedMsiPayload(
      string path,
      string expectedSha256 = null) {
      var fullPath = Path.GetFullPath(path);
      var directory = Path.GetDirectoryName(fullPath);
      if (string.IsNullOrWhiteSpace(directory)) {
        return null;
      }
      var ancestorPins = IsProcessElevated()
        ? PinAndValidateInstallerCacheAncestors(directory)
        : new List<SafeFileHandle>();
      if (ancestorPins == null) {
        return null;
      }
      FileStream stream = null;
      try {
        stream = new FileStream(
          fullPath,
          FileMode.Open,
          FileAccess.Read,
          FileShare.Read);
        ByHandleFileInformation information;
        if (!GetFileInformationByHandle(stream.SafeFileHandle, out information) ||
            (information.FileAttributes & FileAttributeReparsePoint) != 0 ||
            information.NumberOfLinks != 1 ||
            !HandlePathMatches(stream.SafeFileHandle, fullPath)) {
          throw new InvalidDataException("The MSI payload identity is unsafe.");
        }
        var actualSha256 = ComputePinnedStreamSha256Hex(stream);
        if (string.IsNullOrWhiteSpace(expectedSha256)) {
          lock (ApprovedPayloadLock) {
            ApprovedPayloadHashes.TryGetValue(fullPath, out expectedSha256);
          }
        }
        if (string.IsNullOrWhiteSpace(expectedSha256) ||
            !string.Equals(actualSha256, expectedSha256, StringComparison.OrdinalIgnoreCase)) {
          throw new InvalidDataException("The MSI payload digest changed.");
        }
        return new PinnedMsiPayload(fullPath, actualSha256, stream, ancestorPins);
      } catch {
        if (stream != null) {
          stream.Dispose();
        }
        foreach (var pin in ancestorPins) {
          pin.Dispose();
        }
        return null;
      }
    }

    private static PinnedMsiPayload OpenPinnedReleasedMsiPayload(
      string path,
      string expectedProductCode) {
      if (string.IsNullOrWhiteSpace(path)) {
        return null;
      }
      var normalizedExpectedProductCode = NormalizeProductCode(expectedProductCode);
      if (!LooksLikeProductCode(normalizedExpectedProductCode)) {
        return null;
      }
      var fullPath = Path.GetFullPath(path);
      var directory = Path.GetDirectoryName(fullPath);
      var windows = Path.GetFullPath(
        Environment.GetFolderPath(Environment.SpecialFolder.Windows)).TrimEnd(
          Path.DirectorySeparatorChar,
          Path.AltDirectorySeparatorChar);
      var installerCache = Path.Combine(windows, "Installer");
      if (string.IsNullOrWhiteSpace(directory) ||
          (!string.Equals(directory, installerCache, StringComparison.OrdinalIgnoreCase) &&
           !directory.StartsWith(
             installerCache + Path.DirectorySeparatorChar,
             StringComparison.OrdinalIgnoreCase))) {
        return null;
      }
      var ancestorPins = PinAndValidateTrustedWindowsAncestors(
        directory,
        installerCache);
      if (ancestorPins == null) {
        return null;
      }
      FileStream stream = null;
      try {
        stream = new FileStream(
          fullPath,
          FileMode.Open,
          FileAccess.Read,
          FileShare.Read);
        ByHandleFileInformation information;
        var security = new FileInfo(fullPath).GetAccessControl(
          AccessControlSections.Access | AccessControlSections.Owner);
        if (!GetFileInformationByHandle(stream.SafeFileHandle, out information) ||
            (information.FileAttributes & FileAttributeReparsePoint) != 0 ||
            information.NumberOfLinks != 1 ||
            !HandlePathMatches(stream.SafeFileHandle, fullPath) ||
            !InstallerCacheSecurityIsSafe(security, true, false)) {
          throw new InvalidDataException("The cached MSI identity is unsafe.");
        }
        var sha256 = ComputePinnedStreamSha256Hex(stream);
        if (!MsiPackageContainsNativeTrustBoundary(fullPath)) {
          throw new InvalidDataException("The cached MSI is not an approved released artifact.");
        }
        var payloadInfo = TryGetPayloadMsiInfo(fullPath);
        var actualProductCode = NormalizeProductCode(
          payloadInfo == null ? null : payloadInfo.ProductCode);
        if (!string.Equals(
              actualProductCode,
              normalizedExpectedProductCode,
              StringComparison.OrdinalIgnoreCase)) {
          throw new InvalidDataException("The cached MSI ProductCode does not match its registration.");
        }
        return new PinnedMsiPayload(
          fullPath,
          sha256,
          stream,
          ancestorPins,
          actualProductCode);
      } catch {
        if (stream != null) {
          stream.Dispose();
        }
        foreach (var pin in ancestorPins) {
          pin.Dispose();
        }
        return null;
      }
    }

    private static string StageDeveloperMsiOverride(string overridePath) {
      var explicitPath = Path.GetFullPath(overridePath);
      if (!File.Exists(explicitPath)) {
        throw new FileNotFoundException("Specified MSI payload was not found.", explicitPath);
      }
      var embeddedPath = ExtractEmbeddedMsi(true, false);
      var expectedBinarySha256 = ComputeMsiBinaryStreamSha256Hex(embeddedPath);
      if (string.IsNullOrWhiteSpace(expectedBinarySha256)) {
        throw new InvalidDataException("The embedded MSI trust-boundary digest is unavailable.");
      }
      using (var source = new FileStream(
        explicitPath,
        FileMode.Open,
        FileAccess.Read,
        FileShare.Read)) {
        ByHandleFileInformation information;
        if (!GetFileInformationByHandle(source.SafeFileHandle, out information) ||
            (information.FileAttributes & FileAttributeReparsePoint) != 0 ||
            information.NumberOfLinks != 1 ||
            !HandlePathMatches(source.SafeFileHandle, explicitPath)) {
          throw new InvalidDataException(
            "The developer MSI source identity is unsafe.");
        }
        var sourceHash = ComputePinnedStreamSha256Hex(source);
        var destinationDirectory = BuildEmbeddedMsiExtractDirectory(sourceHash, true);
        CreateProtectedInstallerCacheDirectory(destinationDirectory);
        var destination = Path.Combine(destinationDirectory, "Vibepollo.developer.msi");
        source.Position = 0;
        WriteStreamAtomically(source, destination);
        RegisterApprovedPayload(destination, sourceHash);
        using (var staged = OpenPinnedMsiPayload(destination, sourceHash)) {
          if (staged == null ||
              !MsiPackageContainsNativeTrustBoundary(
                destination,
                expectedBinarySha256,
                false)) {
            throw new InvalidDataException("The staged developer MSI changed during validation.");
          }
        }
        return destination;
      }
    }

    private static string ResolveMsiPath(string overridePath, bool forceFreshExtract = false) {
      if (!string.IsNullOrWhiteSpace(overridePath)) {
        if (!IsProcessElevated()) {
          throw new InvalidOperationException(
            "A developer MSI override is accepted only from an already elevated process.");
        }
        return StageDeveloperMsiOverride(overridePath);
      }

      return ValidateResolvedMsiPayload(ExtractEmbeddedMsi(forceFreshExtract, true));
    }

    private static string ValidateResolvedMsiPayload(string msiPath) {
      var payloadInfo = TryGetPayloadMsiInfo(msiPath);
      if (payloadInfo == null || !LooksLikeProductCode(payloadInfo.ProductCode)) {
        throw new InvalidDataException(
          "The MSI payload could not be opened or did not contain a valid ProductCode: " + msiPath);
      }
      return msiPath;
    }

    private static string ExtractEmbeddedMsi(
      bool forceFreshExtract = false,
      bool requireReleasedSigner = true) {
      if (requireReleasedSigner && !IsAuthenticodeTrustValid(
            Assembly.GetExecutingAssembly().Location)) {
        throw new InvalidDataException(
          "The bootstrapper Authenticode signature is missing or invalid.");
      }
      using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Payload.msi")) {
        if (stream == null) {
          throw new InvalidOperationException(
            "No MSI payload was found. The installer may be corrupted.\n\n"
            + "Try re-downloading the installer from the Vibepollo releases page.");
        }

        var versionToken = ComputeStreamSha256Hex(stream);
        var extractDirectory = BuildEmbeddedMsiExtractDirectory(versionToken, forceFreshExtract);
        CreateProtectedInstallerCacheDirectory(extractDirectory);

        var msiPath = Path.Combine(extractDirectory, "Vibepollo.msi");
        WriteStreamAtomically(stream, msiPath);
        RegisterApprovedPayload(msiPath, versionToken);

        using (var pinned = OpenPinnedMsiPayload(msiPath, versionToken)) {
          if (pinned == null || !MsiPackageContainsNativeTrustBoundary(
                msiPath,
                null,
                requireReleasedSigner)) {
          throw new InvalidOperationException(
              "The exact embedded MSI payload did not pass its native trust-boundary inspection.");
          }
        }

        return msiPath;
      }
    }

    private static string BuildEmbeddedMsiExtractDirectory(string versionToken, bool forceFreshExtract) {
      var token = string.IsNullOrWhiteSpace(versionToken)
        ? "unknown"
        : versionToken.Substring(0, Math.Min(16, versionToken.Length));
      return Path.Combine(
        GetEmbeddedMsiExtractRoot(),
        "payload_" + token + "_" + Guid.NewGuid().ToString("N"));
    }

    private static string GetEmbeddedMsiExtractRoot() {
      if (IsProcessElevated()) {
        return GetTrustedInstallerCacheRoot();
      }

      return Path.Combine(Path.GetTempPath(), "VibepolloInstaller");
    }

    private static IEnumerable<string> GetEmbeddedMsiExtractRoots() {
      var roots = new List<string> {
        Path.Combine(Path.GetTempPath(), "VibepolloInstaller")
      };

      if (IsProcessElevated()) {
        roots.Add(GetTrustedInstallerCacheRoot());
      }

      return roots;
    }

    private static void WriteStreamAtomically(Stream input, string destinationPath) {
      if (input == null) {
        throw new InvalidOperationException("The embedded MSI payload could not be read.");
      }

      var destinationDirectory = Path.GetDirectoryName(destinationPath);
      if (string.IsNullOrWhiteSpace(destinationDirectory)) {
        throw new InvalidOperationException("The MSI extraction directory is invalid.");
      }

      CreateProtectedInstallerCacheDirectory(destinationDirectory);
      var tempPath = destinationPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
      try {
        input.Position = 0;
        using (var output = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
          input.CopyTo(output);
          output.Flush(true);
        }
        SetProtectedInstallerCacheFileSecurity(tempPath);
        File.Move(tempPath, destinationPath);
      } finally {
        TryDeleteFile(tempPath);
      }
    }

    private static string ComputeStreamSha256Hex(Stream stream) {
      if (stream == null) {
        return "unknown";
      }

      if (stream.CanSeek) {
        var originalPosition = stream.Position;
        try {
          using (var hasher = SHA256.Create()) {
            var hash = hasher.ComputeHash(stream);
            return string.Concat(hash.Select(b => b.ToString("x2")));
          }
        } finally {
          stream.Position = originalPosition;
        }
      }

      using (var hasher = SHA256.Create()) {
        var hash = hasher.ComputeHash(stream);
        return string.Concat(hash.Select(b => b.ToString("x2")));
      }
    }

    private static string ComputeFileSha256Hex(string path) {
      using (var stream = File.OpenRead(path)) {
        return ComputeStreamSha256Hex(stream);
      }
    }

    private static bool FileHashMatches(string path, string expectedSha256Hex) {
      if (string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(expectedSha256Hex) || !File.Exists(path)) {
        return false;
      }

      try {
        return string.Equals(
          ComputeFileSha256Hex(path),
          expectedSha256Hex,
          StringComparison.OrdinalIgnoreCase);
      } catch {
        return false;
      }
    }

    private static bool WaitForMsiPackageAvailability(string msiPath, int attempts, int delayMs) {
      if (string.IsNullOrWhiteSpace(msiPath) || attempts <= 0) {
        return false;
      }

      for (var attempt = 0; attempt < attempts; attempt++) {
        if (CanOpenMsiPackage(msiPath)) {
          return true;
        }

        if (delayMs > 0 && attempt + 1 < attempts) {
          Thread.Sleep(delayMs);
        }
      }

      return false;
    }

    private static bool IsOperationSwitch(string value) {
      return OperationTokens.Contains(value, StringComparer.OrdinalIgnoreCase);
    }

    private static string TryInjectDefaultMsi(List<string> cliArgs, InstallerArguments arguments) {
      var operationIndex = cliArgs.FindIndex(IsOperationSwitch);
      if (operationIndex < 0) {
        return null;
      }

      var operation = cliArgs[operationIndex];
      var hasValueAfterOperation = operationIndex + 1 < cliArgs.Count && !LooksLikeSwitch(cliArgs[operationIndex + 1]);
      if (hasValueAfterOperation) {
        return null;
      }

      if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/a", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase)) {
        return null;
      }

      var resolvedMsiPath = ResolveMsiPath(arguments.MsiPathOverride);
      cliArgs.Insert(operationIndex + 1, resolvedMsiPath);
      return resolvedMsiPath;
    }

    private static bool TryResolveMsiUninstallTarget(
      IReadOnlyList<string> arguments,
      out int targetIndex,
      out string productCode) {
      targetIndex = -1;
      productCode = string.Empty;
      if (arguments == null) {
        return false;
      }

      for (var index = 0; index < arguments.Count; index++) {
        if (!string.Equals(arguments[index], "/x", StringComparison.OrdinalIgnoreCase)) {
          continue;
        }
        if (index + 1 >= arguments.Count || LooksLikeSwitch(arguments[index + 1])) {
          return false;
        }

        targetIndex = index + 1;
        var candidate = (arguments[targetIndex] ?? string.Empty).Trim().Trim('"');
        var normalizedProductCode = NormalizeProductCode(candidate);
        if (LooksLikeProductCode(normalizedProductCode)) {
          productCode = normalizedProductCode;
          return true;
        }

        try {
          var fullPath = Path.GetFullPath(candidate);
          if (!File.Exists(fullPath)) {
            return false;
          }
          var payloadInfo = TryGetPayloadMsiInfo(fullPath);
          normalizedProductCode = NormalizeProductCode(payloadInfo == null ? null : payloadInfo.ProductCode);
          if (!LooksLikeProductCode(normalizedProductCode)) {
            return false;
          }
          productCode = normalizedProductCode;
          return true;
        } catch {
          return false;
        }
      }

      return false;
    }

    private static bool ReplaceMsiUninstallTarget(
      List<string> arguments,
      string expectedProductCode,
      string replacementMsiPath) {
      if (arguments == null || string.IsNullOrWhiteSpace(replacementMsiPath)) {
        return false;
      }

      int targetIndex;
      string resolvedProductCode;
      if (!TryResolveMsiUninstallTarget(arguments, out targetIndex, out resolvedProductCode)
          || !string.Equals(
            resolvedProductCode,
            NormalizeProductCode(expectedProductCode),
            StringComparison.OrdinalIgnoreCase)) {
        return false;
      }

      arguments[targetIndex] = replacementMsiPath;
      return true;
    }

    private static bool ReplaceArgumentValue(List<string> arguments, string oldValue, string newValue) {
      if (arguments == null || string.IsNullOrWhiteSpace(oldValue) || string.IsNullOrWhiteSpace(newValue)) {
        return false;
      }

      var argumentIndex = arguments.FindIndex(arg => string.Equals(arg, oldValue, StringComparison.OrdinalIgnoreCase));
      if (argumentIndex >= 0) {
        arguments[argumentIndex] = newValue;
        return true;
      }
      return false;
    }

    private static bool LooksLikeSwitch(string value) {
      if (string.IsNullOrWhiteSpace(value)) {
        return true;
      }
      if (value.StartsWith("{", StringComparison.Ordinal) && value.EndsWith("}", StringComparison.Ordinal)) {
        return false;
      }
      if (value.Length >= 2 && value[1] == ':') {
        return false;
      }
      return value.StartsWith("/", StringComparison.Ordinal) || value.StartsWith("-", StringComparison.Ordinal);
    }

    private static bool HasRestartBehavior(List<string> args) {
      return args.Any(arg =>
        string.Equals(arg, "/norestart", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/promptrestart", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/forcerestart", StringComparison.OrdinalIgnoreCase));
    }

    private static bool HasProperty(List<string> args, string propertyName) {
      var prefix = propertyName + "=";
      return args.Any(arg => arg.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    private static int RetryInstallWithSameProductReinstallIfNeeded(
      int exitCode,
      List<string> args,
      string msiPath,
      bool hiddenWindow,
      bool requestElevationIfNeeded) {
      const int anotherVersionInstalled = 1638;
      if (exitCode != anotherVersionInstalled || args == null) {
        return exitCode;
      }
      if (HasProperty(args, "REINSTALL") && HasProperty(args, "REINSTALLMODE")) {
        return exitCode;
      }
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return exitCode;
      }

      var retryArgs = new List<string>(args);
      if (!HasProperty(retryArgs, "REINSTALL")) {
        retryArgs.Add("REINSTALL=ALL");
      }
      if (!HasProperty(retryArgs, "REINSTALLMODE")) {
        retryArgs.Add("REINSTALLMODE=vams");
      }
      return RunMsiexec(retryArgs, hiddenWindow, requestElevationIfNeeded);
    }

    private static void TryAppendSameProductReinstallProperties(List<string> args, string msiPath) {
      if (args == null || string.IsNullOrWhiteSpace(msiPath)) {
        return;
      }
      if (!ShouldUseSameProductReinstall(msiPath)) {
        return;
      }

      if (!HasProperty(args, "REINSTALL")) {
        args.Add("REINSTALL=ALL");
      }
      if (!HasProperty(args, "REINSTALLMODE")) {
        // Use vams (no 'u') to preserve existing HKCU settings during same-product reinstalls.
        args.Add("REINSTALLMODE=vams");
      }
    }

    private static bool ShouldUseSameProductReinstall(string msiPath) {
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return false;
      }

      var payloadInfo = TryGetPayloadMsiInfo(new InstallerArguments {
        MsiPathOverride = msiPath
      });
      if (payloadInfo == null || string.IsNullOrWhiteSpace(payloadInfo.ProductCode)) {
        return false;
      }

      if (IsInstalledProductCode(payloadInfo.ProductCode)) {
        return true;
      }

      return GetInstalledProducts(true).Any(product =>
        !string.IsNullOrWhiteSpace(product.ProductCode) &&
        string.Equals(product.ProductCode, payloadInfo.ProductCode, StringComparison.OrdinalIgnoreCase));
    }

    private static string GetPropertyValue(List<string> args, string propertyName) {
      if (args == null || string.IsNullOrWhiteSpace(propertyName)) {
        return string.Empty;
      }

      var prefix = propertyName + "=";
      var argument = args.FirstOrDefault(arg => arg.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
      if (string.IsNullOrWhiteSpace(argument)) {
        return string.Empty;
      }

      return argument.Substring(prefix.Length).Trim().Trim('"');
    }

    private static bool HasLogSwitch(List<string> args) {
      return args.Any(arg =>
        arg.StartsWith("/l", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/log", StringComparison.OrdinalIgnoreCase));
    }

    private static void InsertMsiSwitchAfterOperation(List<string> args, string switchName, string switchValue) {
      if (args == null) {
        return;
      }

      var insertIndex = 0;
      var operationIndex = args.FindIndex(IsOperationSwitch);
      if (operationIndex >= 0) {
        insertIndex = operationIndex + 1;
        if (insertIndex < args.Count && !LooksLikeSwitch(args[insertIndex])) {
          insertIndex++;
        }
      }

      args.Insert(insertIndex, switchName);
      args.Insert(insertIndex + 1, switchValue);
    }

    private static string TryGetMsiLogPath(List<string> args) {
      if (args == null) {
        return string.Empty;
      }

      for (var index = 0; index < args.Count; index++) {
        var arg = args[index] ?? string.Empty;
        if (string.Equals(arg, "/log", StringComparison.OrdinalIgnoreCase)) {
          if (index + 1 < args.Count) {
            return args[index + 1] ?? string.Empty;
          }
          return string.Empty;
        }
        if (!arg.StartsWith("/l", StringComparison.OrdinalIgnoreCase)) {
          continue;
        }
        if (arg.Length > 2 && !arg.StartsWith("/log", StringComparison.OrdinalIgnoreCase)) {
          var compactPathIndex = arg.IndexOf(':');
          if (compactPathIndex > 0 && compactPathIndex - 1 < arg.Length) {
            return arg.Substring(compactPathIndex - 1).Trim().Trim('"');
          }
        }
        if (index + 1 < args.Count && !LooksLikeSwitch(args[index + 1])) {
          return args[index + 1] ?? string.Empty;
        }
      }

      return string.Empty;
    }

    private static bool ShouldPreUninstallCompetingProducts(List<string> args) {
      var operation = args.FirstOrDefault(IsOperationSwitch);
      if (string.IsNullOrWhiteSpace(operation)) {
        return false;
      }

      return string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
        || string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ShouldPreUninstallProblematicUpgradeSource(List<string> args) {
      var operation = args.FirstOrDefault(IsOperationSwitch);
      if (string.IsNullOrWhiteSpace(operation)) {
        return false;
      }

      return string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
        || string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ShouldPreUninstallVibeshineInstallSource(List<string> args) {
      return IsMsiInstallOperation(args);
    }

    private static string GetMsiPathArgument(List<string> args) {
      if (args == null) {
        return null;
      }

      var operationIndex = args.FindIndex(IsOperationSwitch);
      if (operationIndex < 0 || operationIndex + 1 >= args.Count) {
        return null;
      }

      var operation = args[operationIndex];
      if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase)
          && !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase)) {
        return null;
      }

      var candidate = args[operationIndex + 1];
      return LooksLikeSwitch(candidate) ? null : candidate;
    }

    private static string BuildLogPath(string phase) {
      var timestamp = DateTime.UtcNow.ToString("yyyyMMdd_HHmmss");
      return Path.Combine(Path.GetTempPath(), "vibeshine_" + phase + "_" + timestamp + ".log");
    }

    private static List<string> CollectInstallComponentFailures(string installLogPath, bool installVirtualDisplayDriver) {
      var failures = new List<string>();
      if (!installVirtualDisplayDriver || string.IsNullOrWhiteSpace(installLogPath) || !File.Exists(installLogPath)) {
        return failures;
      }

      try {
        var lines = File.ReadAllLines(installLogPath);
        var virtualDisplayDriverFailed = lines.Any(line =>
          !string.IsNullOrWhiteSpace(line)
          && line.IndexOf("CustomAction InstallVirtualDisplayDriver returned actual error code", StringComparison.OrdinalIgnoreCase) >= 0);
        var virtualDisplayDriverRestartRequired = lines.Any(line =>
          !string.IsNullOrWhiteSpace(line)
          && line.IndexOf("VIRTUAL_DISPLAY_RESTART_REQUIRED", StringComparison.OrdinalIgnoreCase) >= 0);
        var virtualDisplayDriverWarning = lines.Any(line =>
          !string.IsNullOrWhiteSpace(line)
          && line.IndexOf("VIRTUAL_DISPLAY_DRIVER_WARNING", StringComparison.OrdinalIgnoreCase) >= 0);
        if (!virtualDisplayDriverFailed && !virtualDisplayDriverRestartRequired && !virtualDisplayDriverWarning) {
          return failures;
        }

        failures.Add(virtualDisplayDriverRestartRequired
          ? "Virtual display driver installed, but Windows restart is required before virtual display can function."
          : "Virtual display driver setup failed. Virtual display may be unavailable.");
        var detail = ExtractVirtualDisplayDriverFailureDetail(lines);
        if (!string.IsNullOrWhiteSpace(detail)) {
          failures.Add("Driver detail: " + detail);
        }
      } catch {
        // Keep install success semantics even if warning extraction fails.
      }

      return failures;
    }

    private static bool InstallLogIndicatesDriverRebootRequired(string installLogPath) {
      if (string.IsNullOrWhiteSpace(installLogPath) || !File.Exists(installLogPath)) {
        return false;
      }

      try {
        return File.ReadLines(installLogPath).Any(line =>
          !string.IsNullOrWhiteSpace(line)
          && (line.IndexOf("VIRTUAL_DISPLAY_RESTART_REQUIRED", StringComparison.OrdinalIgnoreCase) >= 0
            || line.IndexOf("[SunshineVirtualDisplay] A reboot is required", StringComparison.OrdinalIgnoreCase) >= 0
            || line.IndexOf("[SudoVDA] A reboot is required", StringComparison.OrdinalIgnoreCase) >= 0));
      } catch {
        return false;
      }
    }

    private static string ExtractVirtualDisplayDriverFailureDetail(string[] lines) {
      if (lines == null || lines.Length == 0) {
        return string.Empty;
      }

      for (var index = lines.Length - 1; index >= 0; index--) {
        var line = lines[index];
        if (string.IsNullOrWhiteSpace(line)) {
          continue;
        }

        var isWixOutput = line.IndexOf("WixQuietExec:", StringComparison.OrdinalIgnoreCase) >= 0;
        if (!isWixOutput) {
          continue;
        }

        var isErrorMarker =
          line.IndexOf("Error 0x", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("QuietExec Failed", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("ExecCommon method", StringComparison.OrdinalIgnoreCase) >= 0;
        if (isErrorMarker) {
          continue;
        }

        var looksRelevant =
          line.IndexOf("[SunshineVirtualDisplay]", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("Failed to", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("Unable to", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("Required driver artifact", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("Exception calling", StringComparison.OrdinalIgnoreCase) >= 0
          || line.IndexOf("invalid", StringComparison.OrdinalIgnoreCase) >= 0;
        if (!looksRelevant) {
          continue;
        }

        return line.Replace("WixQuietExec:", string.Empty).Trim();
      }

      return string.Empty;
    }

    private static string PersistInstallLog(string sourceLogPath, string installDirectory, string phase) {
      if (string.IsNullOrWhiteSpace(sourceLogPath) || !File.Exists(sourceLogPath)) {
        throw new InvalidOperationException("The install log was not found in the temporary folder.");
      }
      if (string.IsNullOrWhiteSpace(installDirectory)) {
        throw new InvalidOperationException("Install directory is not available.");
      }

      var fullInstallDirectory = Path.GetFullPath(installDirectory);
      var logDirectory = Path.Combine(fullInstallDirectory, "config", "logs", "installer");
      Directory.CreateDirectory(logDirectory);

      var timestamp = DateTime.UtcNow.ToString("yyyyMMdd_HHmmss");
      var destinationFileName = "vibeshine_" + phase + "_" + timestamp + ".log";
      var destinationPath = Path.Combine(logDirectory, destinationFileName);
      File.Copy(sourceLogPath, destinationPath, true);
      return destinationPath;
    }

    private static InstallerResult RunElevatedBootstrapperInstall(
      InstallerArguments arguments,
      string installDirectory,
      bool installVirtualDisplayDriver,
      bool saveInstallLogs) {
      if (!string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The bootstrapper will not self-elevate a caller-selected MSI."
        };
      }
      var resultPath = Path.Combine(Path.GetTempPath(), "vibeshine_install_result_" + Guid.NewGuid().ToString("N") + ".txt");
      var elevatedArgs = new List<string> {
        "--internal-elevated-install",
        "--internal-install-path",
        installDirectory,
        "--internal-install-virtual-display-driver",
        installVirtualDisplayDriver ? "1" : "0",
        "--internal-install-save-logs",
        saveInstallLogs ? "1" : "0",
        "--internal-install-result-path",
        resultPath
      };
      var exitCode = RunElevatedBootstrapper(elevatedArgs);
      var snapshot = TryReadInternalInstallResult(resultPath);
      var installLogPath = FindMostRecentLog(Path.GetTempPath(), "vibeshine_install_*.log");
      if (snapshot != null && !string.IsNullOrWhiteSpace(snapshot.LogPath)) {
        installLogPath = snapshot.LogPath;
      }
      TryDeleteFile(resultPath);
      return new InstallerResult {
        Operation = InstallerOperation.Install,
        ExitCode = exitCode,
        Message = snapshot == null
          ? BuildResultMessage("Install", exitCode, installLogPath)
          : string.IsNullOrWhiteSpace(snapshot.Message)
            ? BuildResultMessage("Install", exitCode, installLogPath)
            : snapshot.Message,
        UserDetail = snapshot == null ? string.Empty : snapshot.UserDetail,
        LogPath = installLogPath,
        ComponentFailures = snapshot == null ? new List<string>() : (snapshot.ComponentFailures ?? new List<string>()),
        InstallDeferredForRestart = snapshot != null && snapshot.InstallDeferredForRestart
      };
    }

    private static InstallerResult RunElevatedBootstrapperCli(
      InstallerArguments arguments,
      IReadOnlyList<string> normalizedCliArgs = null) {
      if (!string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = 1603,
          Message = "The bootstrapper will not self-elevate a caller-selected MSI."
        };
      }
      var forwardedArguments = normalizedCliArgs == null
        ? new List<string>(arguments.ForwardedArguments)
        : new List<string>(normalizedCliArgs);
      var elevatedOperation = IsMsiUninstallOperation(forwardedArguments)
        ? InstallerOperation.Uninstall
        : InstallerOperation.Install;
      var resultPath = Path.Combine(Path.GetTempPath(), "vibeshine_cli_result_" + Guid.NewGuid().ToString("N") + ".txt");
      var elevatedArgs = new List<string> {
        "--no-ui",
        "--internal-install-result-path",
        resultPath
      };
      elevatedArgs.AddRange(forwardedArguments);

      var exitCode = RunElevatedBootstrapper(elevatedArgs);
      var snapshot = TryReadInternalInstallResult(resultPath);
      var cliLogPath = FindMostRecentLog(Path.GetTempPath(), "vibeshine_cli*.log");
      if (snapshot != null && !string.IsNullOrWhiteSpace(snapshot.LogPath)) {
        cliLogPath = snapshot.LogPath;
      }
      TryDeleteFile(resultPath);
      var installDeferred = snapshot != null && snapshot.InstallDeferredForRestart;
      return new InstallerResult {
        Operation = elevatedOperation,
        ExitCode = exitCode,
        Message = installDeferred
          ? string.IsNullOrWhiteSpace(snapshot.Message)
            ? "Installation is deferred. Migration cleanup completed and Windows must restart before installation can continue."
            : snapshot.Message
          : snapshot != null && !string.IsNullOrWhiteSpace(snapshot.Message)
            ? snapshot.Message
            : BuildResultMessage("CLI operation", exitCode, cliLogPath),
        UserDetail = snapshot == null ? string.Empty : snapshot.UserDetail,
        LogPath = cliLogPath,
        ComponentFailures = snapshot == null ? new List<string>() : (snapshot.ComponentFailures ?? new List<string>()),
        InstallDeferredForRestart = installDeferred
      };
    }

    private static InstallerResult RunElevatedBootstrapperUninstall(
      InstallerArguments arguments,
      bool factoryResetAppData,
      bool removeVirtualDisplayDriver) {
      if (!string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "MSI overrides are not accepted for uninstall."
        };
      }
      var elevatedArgs = new List<string> {
        "--internal-elevated-uninstall",
        "--internal-uninstall-factory-reset",
        factoryResetAppData ? "1" : "0",
        "--internal-uninstall-remove-virtual-display-driver",
        removeVirtualDisplayDriver ? "1" : "0"
      };
      var exitCode = RunElevatedBootstrapper(elevatedArgs);
      var uninstallLogPath = FindMostRecentLog(Path.GetTempPath(), "vibeshine_uninstall_*.log")
        ?? FindMostRecentLog(Path.GetTempPath(), "vibeshine_uninstall_remove_*.log");
      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = exitCode,
        Message = BuildResultMessage("Uninstall", exitCode, uninstallLogPath),
        LogPath = uninstallLogPath
      };
    }

    private static string FindMostRecentLog(string directory, string pattern) {
      if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) {
        return null;
      }

      try {
        return Directory
          .GetFiles(directory, pattern, SearchOption.TopDirectoryOnly)
          .OrderByDescending(File.GetLastWriteTimeUtc)
          .FirstOrDefault();
      } catch {
        return null;
      }
    }

    internal static void TryWriteInternalInstallResult(string resultPath, InstallerResult result) {
      if (string.IsNullOrWhiteSpace(resultPath) || result == null) {
        return;
      }

      try {
        var failures = result.ComponentFailures == null
          ? string.Empty
          : string.Join("\n", result.ComponentFailures.Where(item => !string.IsNullOrWhiteSpace(item)));
        var lines = new[] {
          "ExitCode=" + result.ExitCode,
          "MessageB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.Message ?? string.Empty)),
          "UserDetailB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.UserDetail ?? string.Empty)),
          "LogPathB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.LogPath ?? string.Empty)),
          "ComponentFailuresB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(failures)),
          "InstallDeferredForRestart=" + (result.InstallDeferredForRestart ? "1" : "0")
        };
        File.WriteAllLines(resultPath, lines, Encoding.UTF8);
      } catch {
      }
    }

    private static InternalInstallResultSnapshot TryReadInternalInstallResult(string resultPath) {
      if (string.IsNullOrWhiteSpace(resultPath) || !File.Exists(resultPath)) {
        return null;
      }

      try {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in File.ReadAllLines(resultPath, Encoding.UTF8)) {
          if (string.IsNullOrWhiteSpace(line)) {
            continue;
          }
          var splitIndex = line.IndexOf('=');
          if (splitIndex <= 0) {
            continue;
          }
          var key = line.Substring(0, splitIndex);
          var value = splitIndex + 1 < line.Length ? line.Substring(splitIndex + 1) : string.Empty;
          map[key] = value;
        }

        int parsedExitCode;
        if (!int.TryParse(map.ContainsKey("ExitCode") ? map["ExitCode"] : "0", out parsedExitCode)) {
          parsedExitCode = 0;
        }

        var failures = DecodeBase64Utf8(map, "ComponentFailuresB64")
          .Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)
          .Select(item => item.Trim())
          .Where(item => item.Length > 0)
          .ToList();

        return new InternalInstallResultSnapshot {
          ExitCode = parsedExitCode,
          Message = DecodeBase64Utf8(map, "MessageB64"),
          UserDetail = DecodeBase64Utf8(map, "UserDetailB64"),
          LogPath = DecodeBase64Utf8(map, "LogPathB64"),
          ComponentFailures = failures,
          InstallDeferredForRestart =
            map.ContainsKey("InstallDeferredForRestart")
            && map["InstallDeferredForRestart"] == "1"
        };
      } catch {
        return null;
      }
    }

    private static string DecodeBase64Utf8(IDictionary<string, string> values, string key) {
      if (values == null || string.IsNullOrWhiteSpace(key) || !values.ContainsKey(key)) {
        return string.Empty;
      }

      var raw = values[key] ?? string.Empty;
      if (raw.Length == 0) {
        return string.Empty;
      }

      try {
        return Encoding.UTF8.GetString(Convert.FromBase64String(raw));
      } catch {
        return string.Empty;
      }
    }

    private static void TryDeleteFile(string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return;
      }
      try {
        if (File.Exists(path)) {
          File.Delete(path);
        }
      } catch {
      }
    }

    private static int RunElevatedBootstrapper(IReadOnlyList<string> arguments) {
      var executablePath = Assembly.GetExecutingAssembly().Location;
      var startInfo = new ProcessStartInfo {
        FileName = executablePath,
        Arguments = BuildCommandLine(arguments),
        UseShellExecute = true,
        Verb = "runas",
        WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
      };

      try {
        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return 1;
          }
          process.WaitForExit();
          return process.ExitCode;
        }
      } catch (Win32Exception ex) {
        if (ex.NativeErrorCode == 1223) {
          return 1223;
        }
        return ex.NativeErrorCode == 0 ? 1 : ex.NativeErrorCode;
      }
    }

    private static PinnedMsiPayload OpenPinnedMsiForExecution(
      IReadOnlyList<string> arguments) {
      if (arguments == null) {
        return null;
      }
      for (var index = 0; index < arguments.Count; index++) {
        var operation = arguments[index];
        if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(operation, "/f", StringComparison.OrdinalIgnoreCase)) {
          continue;
        }
        if (index + 1 >= arguments.Count || LooksLikeSwitch(arguments[index + 1])) {
          return null;
        }
        var target = (arguments[index + 1] ?? string.Empty).Trim().Trim('"');
        var productCode = NormalizeProductCode(target);
        if ((string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase) ||
             string.Equals(operation, "/f", StringComparison.OrdinalIgnoreCase)) &&
            LooksLikeProductCode(productCode)) {
          return OpenPinnedReleasedMsiPayload(
            TryGetProductLocalPackagePath(productCode),
            productCode);
        }

        string expectedSha256;
        var fullPath = Path.GetFullPath(target);
        lock (ApprovedPayloadLock) {
          ApprovedPayloadHashes.TryGetValue(fullPath, out expectedSha256);
        }
        return string.IsNullOrWhiteSpace(expectedSha256)
          ? null
          : OpenPinnedMsiPayload(fullPath, expectedSha256);
      }
      return null;
    }

    private static PinnedMsiPayload OpenPinnedRegisteredMaintenancePayload(
      IReadOnlyList<string> arguments) {
      if (!IsRegisteredMaintenanceOperation(arguments)) {
        return null;
      }
      return OpenPinnedMsiForExecution(arguments);
    }

    private static bool TryValidatePinnedMsiExecutionTarget(
      IReadOnlyList<string> arguments,
      PinnedMsiPayload pinnedPayload,
      out List<string> validatedArguments) {
      validatedArguments = null;
      if (pinnedPayload == null || string.IsNullOrWhiteSpace(pinnedPayload.Path)) {
        return false;
      }

      int operationIndex;
      int targetIndex;
      string operation;
      string target;
      if (!TryGetSoleMsiOperationTarget(
            arguments,
            out operationIndex,
            out operation,
            out targetIndex,
            out target)) {
        return false;
      }
      for (var index = 0; index < arguments.Count; index++) {
        if (index == operationIndex || index == targetIndex) {
          continue;
        }
        var token = (arguments[index] ?? string.Empty).Trim().Trim('"');
        if (IsOperationSwitch(token) || LooksLikeAdditionalMsiTarget(token)) {
          return false;
        }
      }

      validatedArguments = new List<string>(arguments);
      if (string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase) ||
          string.Equals(operation, "/f", StringComparison.OrdinalIgnoreCase)) {
        var normalizedProductCode = NormalizeProductCode(target);
        if (!LooksLikeProductCode(normalizedProductCode) ||
            !string.Equals(
              normalizedProductCode,
              pinnedPayload.ProductCode,
              StringComparison.OrdinalIgnoreCase)) {
          validatedArguments = null;
          return false;
        }
        string registeredPath;
        try {
          registeredPath = Path.GetFullPath(
            TryGetProductLocalPackagePath(normalizedProductCode));
        } catch {
          validatedArguments = null;
          return false;
        }
        if (!string.Equals(
              registeredPath,
              pinnedPayload.Path,
              StringComparison.OrdinalIgnoreCase)) {
          validatedArguments = null;
          return false;
        }

        // Execute the exact cache file held by pinnedPayload rather than asking
        // msiexec to resolve the mutable ProductCode registration a second time.
        validatedArguments[targetIndex] = pinnedPayload.Path;
        return true;
      }

      if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase)) {
        validatedArguments = null;
        return false;
      }
      string targetPath;
      try {
        targetPath = Path.GetFullPath(target);
      } catch {
        validatedArguments = null;
        return false;
      }
      if (!string.Equals(targetPath, pinnedPayload.Path, StringComparison.OrdinalIgnoreCase)) {
        validatedArguments = null;
        return false;
      }
      validatedArguments[targetIndex] = pinnedPayload.Path;
      return true;
    }

    private static int RunMsiexec(IReadOnlyList<string> arguments, bool hiddenWindow, bool requestElevationIfNeeded) {
      using (var pinnedPayload = OpenPinnedMsiForExecution(arguments)) {
        return RunMsiexec(
          arguments,
          hiddenWindow,
          requestElevationIfNeeded,
          pinnedPayload);
      }
    }

    private static int RunMsiexec(
      IReadOnlyList<string> arguments,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      PinnedMsiPayload pinnedPayload) {
      List<string> validatedArguments;
      if (!TryValidatePinnedMsiExecutionTarget(
            arguments,
            pinnedPayload,
            out validatedArguments)) {
        return 1603;
      }
      return RunProcess(
        ResolveMsiexecPath(),
        BuildCommandLine(validatedArguments),
        hiddenWindow,
        requestElevationIfNeeded,
        MsiExecTimeoutMilliseconds,
        MsiExecTimeoutExitCode,
        TryGetMsiLogPath(validatedArguments));
    }

    private static int RunProcess(string executablePath, string arguments, bool hiddenWindow, bool requestElevationIfNeeded) {
      return RunProcess(executablePath, arguments, hiddenWindow, requestElevationIfNeeded, 0, 0, null);
    }

    private static int RunProcess(
      string executablePath,
      string arguments,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      int timeoutMilliseconds,
      int timeoutExitCode,
      string timeoutLogPath) {
      var shouldElevate = requestElevationIfNeeded && !IsProcessElevated();
      var workingDirectory = AppDomain.CurrentDomain.BaseDirectory;
      try {
        var executableDirectory = Path.GetDirectoryName(NormalizePath(executablePath));
        if (!string.IsNullOrWhiteSpace(executableDirectory) && Directory.Exists(executableDirectory)) {
          workingDirectory = executableDirectory;
        }
      } catch {
      }

      var startInfo = new ProcessStartInfo {
        FileName = executablePath,
        Arguments = arguments ?? string.Empty,
        UseShellExecute = shouldElevate,
        WorkingDirectory = workingDirectory
      };

      if (shouldElevate) {
        startInfo.Verb = "runas";
      } else {
        startInfo.CreateNoWindow = hiddenWindow;
      }

      try {
        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return 1;
          }
          if (timeoutMilliseconds > 0 && !process.WaitForExit(timeoutMilliseconds)) {
            AppendInstallerLogMessage(
              timeoutLogPath,
              Path.GetFileName(executablePath) + " did not exit within "
              + TimeSpan.FromMilliseconds(timeoutMilliseconds).TotalMinutes.ToString("0")
              + " minutes; terminating process " + process.Id + ".");
            try {
              process.Kill();
            } catch {
            }
            try {
              process.WaitForExit(10000);
            } catch {
            }
            return timeoutExitCode;
          }
          process.WaitForExit();
          return process.ExitCode;
        }
      } catch (Win32Exception ex) {
        if (ex.NativeErrorCode == 1223) {
          return 1223;
        }
        throw;
      }
    }

    private static string ResolveMsiexecPath() {
      var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
      if (Environment.Is64BitOperatingSystem && !Environment.Is64BitProcess) {
        return Path.Combine(windowsDirectory, "Sysnative", "msiexec.exe");
      }
      return Path.Combine(windowsDirectory, "System32", "msiexec.exe");
    }

    private static string BuildCommandLine(IEnumerable<string> arguments) {
      return string.Join(" ", arguments.Select(QuoteArgument));
    }

    private static string QuoteArgument(string argument) {
      if (string.IsNullOrEmpty(argument)) {
        return "\"\"";
      }
      string msiPropertyArgument;
      if (TryQuoteMsiPropertyArgument(argument, out msiPropertyArgument)) {
        return msiPropertyArgument;
      }
      if (argument.IndexOfAny(new[] {' ', '\t', '"'}) < 0) {
        return argument;
      }

      var builder = new StringBuilder();
      builder.Append('"');
      var backslashes = 0;
      foreach (var ch in argument) {
        if (ch == '\\') {
          backslashes++;
          continue;
        }

        if (ch == '"') {
          builder.Append('\\', backslashes * 2 + 1);
          builder.Append('"');
          backslashes = 0;
          continue;
        }

        if (backslashes > 0) {
          builder.Append('\\', backslashes);
          backslashes = 0;
        }
        builder.Append(ch);
      }

      if (backslashes > 0) {
        builder.Append('\\', backslashes * 2);
      }
      builder.Append('"');
      return builder.ToString();
    }

    private static string CreatePropertyArgument(string propertyName, string propertyValue) {
      var escaped = (propertyValue ?? string.Empty).Replace("\"", "\"\"");
      return propertyName + "=\"" + escaped + "\"";
    }

    private static bool TryQuoteMsiPropertyArgument(string argument, out string quotedArgument) {
      quotedArgument = null;
      var splitIndex = string.IsNullOrEmpty(argument) ? -1 : argument.IndexOf('=');
      if (splitIndex <= 0 || !IsMsiPropertyName(argument.Substring(0, splitIndex))) {
        return false;
      }

      var propertyName = argument.Substring(0, splitIndex);
      var propertyValue = argument.Substring(splitIndex + 1);
      if (propertyValue.Length >= 2
          && propertyValue[0] == '"'
          && propertyValue[propertyValue.Length - 1] == '"') {
        quotedArgument = argument;
        return true;
      }

      if (propertyValue.IndexOfAny(new[] {' ', '\t', '"'}) < 0) {
        return false;
      }

      quotedArgument = propertyName + "=\"" + propertyValue.Replace("\"", "\"\"") + "\"";
      return true;
    }

    private static bool IsMsiPropertyName(string propertyName) {
      if (string.IsNullOrEmpty(propertyName)) {
        return false;
      }

      foreach (var ch in propertyName) {
        if ((ch < 'A' || ch > 'Z') && (ch < '0' || ch > '9') && ch != '_') {
          return false;
        }
      }

      return true;
    }

    private static bool IsProcessElevated() {
      using (var identity = WindowsIdentity.GetCurrent()) {
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
      }
    }

    private static string BuildResultMessage(string operationName, int exitCode, string logPath) {
      if (exitCode == 0 || exitCode == 3010) {
        return operationName + " succeeded.";
      }
      if (exitCode == 1223) {
        return operationName + " cancelled.";
      }

      var message = operationName + " failed (error " + exitCode + ").";
      if (exitCode == 1603) {
        message += " A fatal error occurred during installation. Ensure no Vibepollo processes are running and try again.";
      } else if (exitCode == 1618) {
        message += " Another installation is already in progress. Wait for it to finish, then try again.";
      } else if (exitCode == 1602) {
        message += " The installation was cancelled by the user.";
      } else if (exitCode == 1605) {
        message += " No existing Vibepollo installation was found.";
      }
      if (!string.IsNullOrWhiteSpace(logPath)) {
        message += " Log: " + logPath;
      }
      return message;
    }
  }

  internal static class ShellIdentity {
    internal const string InstallerAppUserModelId = "Vibepollo.Installer";

    private static readonly PropertyKey AppUserModelIdKey =
      new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 5);
    private static readonly PropertyKey RelaunchCommandKey =
      new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 2);
    private static readonly PropertyKey RelaunchDisplayNameKey =
      new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 4);
    private static readonly PropertyKey RelaunchIconKey =
      new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 3);
    private static readonly Guid PropertyStoreGuid =
      new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");

    [ComImport]
    [Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore {
      [PreserveSig] int GetCount(out uint cProps);
      [PreserveSig] int GetAt(uint iProp, out PropertyKey pkey);
      [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant pv);
      [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant pv);
      [PreserveSig] int Commit();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PropertyKey {
      public Guid FormatId;
      public uint PropertyId;

      public PropertyKey(Guid formatId, uint propertyId) {
        FormatId = formatId;
        PropertyId = propertyId;
      }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PropVariant : IDisposable {
      private ushort _valueType;
      private ushort _reserved1;
      private ushort _reserved2;
      private ushort _reserved3;
      private IntPtr _pointerValue;
      private int _int32Value;

      public static PropVariant FromString(string value) {
        return new PropVariant {
          _valueType = 31,
          _pointerValue = Marshal.StringToCoTaskMemUni(value ?? string.Empty)
        };
      }

      public void Dispose() {
        PropVariantClear(ref this);
      }
    }

    [DllImport("shell32.dll")]
    private static extern int SHGetPropertyStoreForWindow(
      IntPtr hwnd,
      [In] ref Guid iid,
      [Out, MarshalAs(UnmanagedType.Interface)] out IPropertyStore propertyStore
    );

    [DllImport("ole32.dll")]
    private static extern int PropVariantClear(ref PropVariant propVariant);

    internal static void TryApplyInstallerWindowIdentity(IntPtr windowHandle, string displayName) {
      if (windowHandle == IntPtr.Zero) {
        return;
      }

      try {
        IPropertyStore propertyStore;
        var propertyStoreGuid = PropertyStoreGuid;
        var hr = SHGetPropertyStoreForWindow(windowHandle, ref propertyStoreGuid, out propertyStore);
        if (hr != 0 || propertyStore == null) {
          return;
        }

        try {
          SetStringProperty(propertyStore, AppUserModelIdKey, InstallerAppUserModelId);

          if (!string.IsNullOrWhiteSpace(displayName)) {
            SetStringProperty(propertyStore, RelaunchDisplayNameKey, displayName);
          }

          var executablePath = GetExecutablePath();
          if (!string.IsNullOrWhiteSpace(executablePath)) {
            SetStringProperty(propertyStore, RelaunchCommandKey, QuoteForCommandLine(executablePath));
            SetStringProperty(propertyStore, RelaunchIconKey, executablePath + ",0");
          }

          propertyStore.Commit();
        } finally {
          Marshal.FinalReleaseComObject(propertyStore);
        }
      } catch {
      }
    }

    private static void SetStringProperty(IPropertyStore propertyStore, PropertyKey key, string value) {
      var propVariant = PropVariant.FromString(value);
      try {
        propertyStore.SetValue(ref key, ref propVariant);
      } finally {
        propVariant.Dispose();
      }
    }

    private static string GetExecutablePath() {
      try {
        var entryAssembly = Assembly.GetEntryAssembly();
        if (entryAssembly != null && !string.IsNullOrWhiteSpace(entryAssembly.Location)) {
          return Path.GetFullPath(entryAssembly.Location);
        }
      } catch {
      }

      try {
        using (var process = Process.GetCurrentProcess()) {
          if (process.MainModule != null && !string.IsNullOrWhiteSpace(process.MainModule.FileName)) {
            return Path.GetFullPath(process.MainModule.FileName);
          }
        }
      } catch {
      }

      return string.Empty;
    }

    private static string QuoteForCommandLine(string path) {
      return string.IsNullOrWhiteSpace(path) ? string.Empty : "\"" + path + "\"";
    }
  }

  internal static class ModernFolderPicker {
    private const uint FOS_PICKFOLDERS = 0x00000020;
    private const uint FOS_FORCEFILESYSTEM = 0x00000040;
    private const uint FOS_PATHMUSTEXIST = 0x00000800;
    private const uint SIGDN_FILESYSPATH = 0x80058000;

    [ComImport]
    [Guid("42F85136-DB7E-439C-85F1-E4075D135FC8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileDialog {
      [PreserveSig] int Show(IntPtr parent);
      void SetFileTypes(uint cFileTypes, IntPtr rgFilterSpec);
      void SetFileTypeIndex(uint iFileType);
      void GetFileTypeIndex(out uint piFileType);
      void Advise(IntPtr pfde, out uint pdwCookie);
      void Unadvise(uint dwCookie);
      void SetOptions(uint fos);
      void GetOptions(out uint pfos);
      void SetDefaultFolder(IShellItem psi);
      void SetFolder(IShellItem psi);
      void GetFolder(out IShellItem ppsi);
      void GetCurrentSelection(out IShellItem ppsi);
      void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
      void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string pszName);
      void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
      void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
      void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
      void GetResult(out IShellItem ppsi);
      void AddPlace(IShellItem psi, int fdap);
      void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
      void Close(int hr);
      void SetClientGuid(ref Guid guid);
      void ClearClientData();
      void SetFilter(IntPtr pFilter);
    }

    [ComImport]
    [Guid("D57C7288-D4AD-4768-BE02-9D969532D960")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileOpenDialog : IFileDialog {
      void GetResults(out IntPtr ppenum);
      void GetSelectedItems(out IntPtr ppsai);
    }

    [ComImport]
    [Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItem {
      void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
      void GetParent(out IShellItem ppsi);
      void GetDisplayName(uint sigdnName, out IntPtr ppszName);
      void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
      void Compare(IShellItem psi, uint hint, out int piOrder);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHCreateItemFromParsingName([MarshalAs(UnmanagedType.LPWStr)] string pszPath, IntPtr pbc, [In] ref Guid riid, [Out] out IShellItem ppv);

    public static string TryPickFolder(Window owner, string title, string initialPath) {
      object dialogComObject = null;
      try {
        var dialogType = Type.GetTypeFromCLSID(new Guid("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7"));
        if (dialogType == null) {
          return null;
        }

        dialogComObject = Activator.CreateInstance(dialogType);
        var dialog = (IFileOpenDialog)dialogComObject;

        uint options;
        dialog.GetOptions(out options);
        options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        dialog.SetOptions(options);

        if (!string.IsNullOrWhiteSpace(title)) {
          dialog.SetTitle(title);
        }

        var normalizedInitial = NormalizeExistingFolder(initialPath);
        if (!string.IsNullOrWhiteSpace(normalizedInitial)) {
          var iidShellItem = new Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE");
          IShellItem folderItem;
          var hrFolder = SHCreateItemFromParsingName(normalizedInitial, IntPtr.Zero, ref iidShellItem, out folderItem);
          if (hrFolder == 0 && folderItem != null) {
            dialog.SetFolder(folderItem);
          }
        }

        var ownerHandle = owner == null ? IntPtr.Zero : new WindowInteropHelper(owner).Handle;
        var hr = dialog.Show(ownerHandle);
        if (hr != 0) {
          return null;
        }

        IShellItem result;
        dialog.GetResult(out result);
        if (result == null) {
          return null;
        }

        IntPtr pathPtr;
        result.GetDisplayName(SIGDN_FILESYSPATH, out pathPtr);
        if (pathPtr == IntPtr.Zero) {
          return null;
        }

        try {
          return Marshal.PtrToStringUni(pathPtr);
        } finally {
          Marshal.FreeCoTaskMem(pathPtr);
        }
      } catch {
        return null;
      } finally {
        if (dialogComObject != null) {
          try {
            Marshal.FinalReleaseComObject(dialogComObject);
          } catch {
          }
        }
      }
    }

    private static string NormalizeExistingFolder(string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return null;
      }
      try {
        var full = Path.GetFullPath(path);
        if (Directory.Exists(full)) {
          return full;
        }
        var parent = Path.GetDirectoryName(full);
        if (!string.IsNullOrWhiteSpace(parent) && Directory.Exists(parent)) {
          return parent;
        }
      } catch {
      }
      return null;
    }
  }
}
