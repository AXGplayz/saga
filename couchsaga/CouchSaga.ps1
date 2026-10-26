<#
  CouchSaga launcher - LEGO Star Wars: The Complete Saga online co-op.
  Starts saga_native.exe (from the same folder) in host, join, two-controller
  or sync-test mode. Windows PowerShell 5.1, no install needed.
#>
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms

$AppDir   = Split-Path -Parent $PSCommandPath
$GameExe  = Join-Path $AppDir 'saga_native.exe'
$ResDir   = Join-Path $AppDir 'res'
$ObbName  = 'main.1060.com.wb.lego.tcs.obb'
$ObbPath  = Join-Path $ResDir $ObbName
$TestFile = Join-Path $ResDir 'sync-test.tcsr'
$SettingsDir  = Join-Path $env:LOCALAPPDATA 'CouchSaga'
$SettingsFile = Join-Path $SettingsDir 'settings.json'

try {
    Add-Type -Namespace CouchSaga -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@
    [void][CouchSaga.Native]::ShowWindow([CouchSaga.Native]::GetConsoleWindow(), 0)
} catch { }

# ---------- settings ----------
$Settings = [pscustomobject]@{ address = ''; port = 7777; delay = '' }
if (Test-Path $SettingsFile) {
    try {
        $saved = Get-Content $SettingsFile -Raw | ConvertFrom-Json
        foreach ($p in 'address', 'port', 'delay') { if ($saved.PSObject.Properties.Name -contains $p) { $Settings.$p = $saved.$p } }
    } catch { }
}
function Save-Settings {
    try {
        New-Item -ItemType Directory -Force -Path $SettingsDir | Out-Null
        $Settings | ConvertTo-Json | Set-Content $SettingsFile -Encoding UTF8
    } catch { }
}

[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="CouchSaga" Width="900" SizeToContent="Height" ResizeMode="CanMinimize"
        WindowStartupLocation="CenterScreen" Background="#0F1117" FontFamily="Segoe UI">
  <Window.Resources>
    <LinearGradientBrush x:Key="Accent" StartPoint="0,0" EndPoint="1,1">
      <GradientStop Color="#FFB300" Offset="0"/>
      <GradientStop Color="#FF6A00" Offset="1"/>
    </LinearGradientBrush>
    <Style x:Key="Primary" TargetType="Button">
      <Setter Property="Foreground" Value="#1A1200"/>
      <Setter Property="FontSize" Value="14"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Padding" Value="18,11"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Background" Value="{StaticResource Accent}"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="{TemplateBinding Background}" CornerRadius="10" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Opacity" Value="0.88"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="b" Property="Opacity" Value="0.72"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter TargetName="b" Property="Opacity" Value="0.35"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Ghost" TargetType="Button" BasedOn="{StaticResource Primary}">
      <Setter Property="Background" Value="#232838"/>
      <Setter Property="Foreground" Value="#E8EAF0"/>
      <Setter Property="FontWeight" Value="Normal"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="Padding" Value="14,8"/>
    </Style>
    <Style x:Key="Card" TargetType="Border">
      <Setter Property="Background" Value="#171A23"/>
      <Setter Property="BorderBrush" Value="#262A36"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="CornerRadius" Value="14"/>
      <Setter Property="Padding" Value="20,18"/>
      <Setter Property="Margin" Value="0,0,0,16"/>
    </Style>
    <Style x:Key="H" TargetType="TextBlock">
      <Setter Property="FontSize" Value="17"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Foreground" Value="#E8EAF0"/>
      <Setter Property="Margin" Value="0,0,0,6"/>
    </Style>
    <Style x:Key="Sub" TargetType="TextBlock">
      <Setter Property="FontSize" Value="12.5"/>
      <Setter Property="Foreground" Value="#8A90A2"/>
      <Setter Property="TextWrapping" Value="Wrap"/>
      <Setter Property="LineHeight" Value="18"/>
    </Style>
    <Style x:Key="Label" TargetType="TextBlock">
      <Setter Property="FontSize" Value="10"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Foreground" Value="#5D6375"/>
      <Setter Property="Margin" Value="2,10,0,4"/>
    </Style>
    <Style x:Key="Field" TargetType="TextBox">
      <Setter Property="Foreground" Value="#E8EAF0"/>
      <Setter Property="CaretBrush" Value="#E8EAF0"/>
      <Setter Property="FontSize" Value="15"/>
      <Setter Property="FontFamily" Value="Consolas"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="TextBox">
            <Border x:Name="b" Background="#0F1117" BorderBrush="#2A2F3D" BorderThickness="1.5" CornerRadius="10" Padding="10,8">
              <ScrollViewer x:Name="PART_ContentHost"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="#FFB300"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Window.Resources>

  <StackPanel Margin="28,22,28,10">
    <TextBlock Text="CouchSaga" FontSize="30" FontWeight="Bold" Foreground="#E8EAF0"/>
    <TextBlock Style="{StaticResource Sub}" FontSize="13.5" Margin="0,2,0,18"
               Text="LEGO Star Wars: The Complete Saga, two players, two houses. Both games run on their own PCs; only controller presses travel between them."/>

    <Border x:Name="DataBanner" CornerRadius="10" Padding="14,12" Margin="0,0,0,16" Background="#2A2414">
      <Grid>
        <TextBlock x:Name="DataText" Foreground="#FFD58A" FontSize="13" TextWrapping="Wrap" VerticalAlignment="Center" Margin="0,0,180,0"/>
        <Button x:Name="BtnChooseData" Style="{StaticResource Ghost}" HorizontalAlignment="Right" Content="Choose game data file"/>
      </Grid>
    </Border>

    <Grid>
      <Grid.ColumnDefinitions>
        <ColumnDefinition/><ColumnDefinition Width="16"/><ColumnDefinition/>
      </Grid.ColumnDefinitions>

      <Border Style="{StaticResource Card}">
        <StackPanel>
          <TextBlock Text="Host" Style="{StaticResource H}"/>
          <TextBlock Style="{StaticResource Sub}" Text="You are Player 1 and your save file is used. Your friend needs your address, and the port must be open (see below)."/>
          <TextBlock Text="PORT" Style="{StaticResource Label}"/>
          <TextBox x:Name="HostPort" Style="{StaticResource Field}" Width="120" HorizontalAlignment="Left"/>
          <TextBlock Text="YOUR ADDRESSES" Style="{StaticResource Label}"/>
          <TextBlock x:Name="LocalAddresses" Foreground="#C9CDD8" FontFamily="Consolas" FontSize="13" TextWrapping="Wrap"/>
          <Grid Margin="0,8,0,0">
            <TextBlock x:Name="PublicAddress" Foreground="#C9CDD8" FontFamily="Consolas" FontSize="13" VerticalAlignment="Center"/>
            <Button x:Name="BtnPublic" Style="{StaticResource Ghost}" HorizontalAlignment="Right" Content="Find my internet address"/>
          </Grid>
          <Button x:Name="BtnHost" Style="{StaticResource Primary}" Margin="0,16,0,0" Content="Host game"/>
        </StackPanel>
      </Border>

      <Border Grid.Column="2" Style="{StaticResource Card}">
        <StackPanel>
          <TextBlock Text="Join" Style="{StaticResource H}"/>
          <TextBlock Style="{StaticResource Sub}" Text="You are Player 2. The game boots from a copy of your friend's save; your own saves are not touched."/>
          <TextBlock Text="HOST ADDRESS (address or address:port)" Style="{StaticResource Label}"/>
          <TextBox x:Name="JoinAddress" Style="{StaticResource Field}"/>
          <Button x:Name="BtnJoin" Style="{StaticResource Primary}" Margin="0,16,0,0" Content="Join game"/>
          <TextBlock Style="{StaticResource Sub}" Margin="0,14,0,0" Text="Both of you must use the exact same CouchSaga download and the same game data file, or the host will refuse the connection."/>
        </StackPanel>
      </Border>
    </Grid>

    <Border Style="{StaticResource Card}">
      <Grid>
        <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
        <StackPanel Margin="0,0,16,0">
          <TextBlock Text="Same couch" Style="{StaticResource H}"/>
          <TextBlock Style="{StaticResource Sub}" Text="Two controllers on this PC. The second controller plays Player 2."/>
        </StackPanel>
        <Button x:Name="BtnCouch" Grid.Column="1" Style="{StaticResource Ghost}" VerticalAlignment="Center" Content="Play with two controllers"/>
      </Grid>
    </Border>

    <Border Style="{StaticResource Card}">
      <StackPanel>
        <TextBlock Text="Sync test (do this first, on one PC)" Style="{StaticResource H}"/>
        <TextBlock Style="{StaticResource Sub}" Text="Online co-op only works if the game plays out identically on both PCs. 1) Record: play a few minutes, then close the game. 2) Replay: the game replays your inputs by itself and the title bar says whether every checksum matched. If it says DIFFERS, send res\netplay.log to whoever is helping you."/>
        <Grid Margin="0,12,0,0">
          <Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="10"/><ColumnDefinition Width="Auto"/><ColumnDefinition/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
          <Button x:Name="BtnRecord" Style="{StaticResource Ghost}" Content="1. Record"/>
          <Button x:Name="BtnReplay" Grid.Column="2" Style="{StaticResource Ghost}" Content="2. Replay"/>
          <StackPanel Grid.Column="4" Orientation="Horizontal">
            <TextBlock Text="INPUT DELAY" Style="{StaticResource Label}" Margin="0,0,8,0" VerticalAlignment="Center"/>
            <TextBox x:Name="DelayBox" Style="{StaticResource Field}" Width="70" ToolTip="Frames. Leave empty to pick from ping (host only)."/>
            <Button x:Name="BtnLogs" Style="{StaticResource Ghost}" Margin="10,0,0,0" Content="Open logs"/>
          </StackPanel>
        </Grid>
      </StackPanel>
    </Border>

    <TextBlock x:Name="Footer" Style="{StaticResource Sub}" FontSize="11.5" Margin="2,0,0,12"
               Text="Hosting opens one port (TCP + UDP) on your router to this game only, and only while you host. Joining opens nothing. A VPN such as ZeroTier works too: use its address instead."/>
  </StackPanel>
</Window>
'@

$win = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader $xaml))
$ui = @{}
foreach ($n in $xaml.SelectNodes('//*[@*[local-name()="Name"]]')) {
    $name = $n.GetAttribute('Name', 'http://schemas.microsoft.com/winfx/2006/xaml')
    if ($name -and $name -notin 'b', 'PART_ContentHost') { $ui[$name] = $win.FindName($name) }
}

function Brush($hex) { New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString($hex)) }

function Update-DataBanner {
    if (-not (Test-Path $GameExe)) {
        $ui.DataBanner.Background = Brush '#2E1520'
        $ui.DataText.Foreground = Brush '#FF9AAE'
        $ui.DataText.Text = 'saga_native.exe is missing. Keep CouchSaga.ps1 in the folder you unzipped.'
        $ui.BtnChooseData.Visibility = 'Collapsed'
        return $false
    }
    if (Test-Path $ObbPath) {
        $ui.DataBanner.Background = Brush '#11291F'
        $ui.DataText.Foreground = Brush '#7FEABC'
        $ui.DataText.Text = "Game data found: res\$ObbName"
        $ui.BtnChooseData.Content = 'Change'
        return $true
    }
    $ui.DataBanner.Background = Brush '#2A2414'
    $ui.DataText.Foreground = Brush '#FFD58A'
    $ui.DataText.Text = "Game data needed: $ObbName (from your copy of the Android game). It is copied into the res folder once."
    $ui.BtnChooseData.Content = 'Choose game data file'
    return $false
}

function Show-Addresses {
    $list = @()
    try {
        $list = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
            ForEach-Object { "$($_.IPAddress)  ($($_.InterfaceAlias))" }
    } catch { }
    if ($list.Count -eq 0) { $list = @('(none found)') }
    $ui.LocalAddresses.Text = ($list -join "`n")
}

function Start-Game([string[]]$GameArgs) {
    if (-not (Update-DataBanner)) {
        [Windows.MessageBox]::Show('Choose your game data file first.', 'CouchSaga') | Out-Null
        return
    }
    Save-Settings
    $argText = 'window ' + ($GameArgs -join ' ')
    Start-Process -FilePath $GameExe -ArgumentList $argText -WorkingDirectory $AppDir -WindowStyle Minimized
}

function Get-DelayArgs {
    $d = $ui.DelayBox.Text.Trim()
    $Settings.delay = $d
    if ($d -match '^\d+$' -and [int]$d -ge 1 -and [int]$d -le 30) { return @('--delay', $d) }
    return @()
}

$ui.HostPort.Text = [string]$Settings.port
$ui.JoinAddress.Text = [string]$Settings.address
$ui.DelayBox.Text = [string]$Settings.delay
Update-DataBanner | Out-Null
Show-Addresses

$ui.BtnChooseData.Add_Click({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose $ObbName"
    $dialog.Filter = 'Android game data (*.obb)|*.obb|All files (*.*)|*.*'
    if ($dialog.ShowDialog() -ne 'OK') { return }
    try {
        New-Item -ItemType Directory -Force -Path $ResDir | Out-Null
        $win.Cursor = [System.Windows.Input.Cursors]::Wait
        Copy-Item -LiteralPath $dialog.FileName -Destination $ObbPath -Force
    } catch {
        [Windows.MessageBox]::Show("Could not copy the file: $_", 'CouchSaga') | Out-Null
    } finally {
        $win.Cursor = $null
    }
    Update-DataBanner | Out-Null
})

$ui.BtnPublic.Add_Click({
    $ui.PublicAddress.Text = 'looking up...'
    try {
        $ip = (Invoke-RestMethod -Uri 'https://api.ipify.org' -TimeoutSec 8).ToString().Trim()
        $ui.PublicAddress.Text = "$ip (copied)"
        [Windows.Clipboard]::SetText("${ip}:$($ui.HostPort.Text.Trim())")
    } catch {
        $ui.PublicAddress.Text = 'could not look it up'
    }
})

$ui.BtnHost.Add_Click({
    $port = $ui.HostPort.Text.Trim()
    if ($port -notmatch '^\d+$' -or [int]$port -lt 1 -or [int]$port -gt 65535) {
        [Windows.MessageBox]::Show('The port must be a number from 1 to 65535.', 'CouchSaga') | Out-Null
        return
    }
    $Settings.port = [int]$port
    Start-Game (@('--host', '--port', $port) + (Get-DelayArgs))
})

$ui.BtnJoin.Add_Click({
    $address = $ui.JoinAddress.Text.Trim()
    if (-not $address -or $address -match '\s') {
        [Windows.MessageBox]::Show("Type the host's address, for example 203.0.113.7 or 203.0.113.7:7777.", 'CouchSaga') | Out-Null
        return
    }
    $Settings.address = $address
    Start-Game @('--join', $address)
})

$ui.BtnCouch.Add_Click({ Start-Game @('--two-player') })
$ui.BtnRecord.Add_Click({ Start-Game @('--record', "`"$TestFile`"") })
$ui.BtnReplay.Add_Click({
    if (-not (Test-Path $TestFile)) {
        [Windows.MessageBox]::Show('Record a test first.', 'CouchSaga') | Out-Null
        return
    }
    Start-Game @('--replay', "`"$TestFile`"")
})
$ui.BtnLogs.Add_Click({
    New-Item -ItemType Directory -Force -Path $ResDir | Out-Null
    Start-Process explorer.exe $ResDir
})

[void]$win.ShowDialog()
