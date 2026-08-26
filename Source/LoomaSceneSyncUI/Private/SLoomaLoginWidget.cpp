#include "SLoomaLoginWidget.h"

#include "LoomaAuthTypes.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
// SVerticalBox / SHorizontalBox. Arrives transitively today, but named explicitly:
// leaning on a transitive include is how a build breaks on an unrelated engine upgrade.
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "LoomaLogin"

void SLoomaLoginWidget::Construct(const FArguments& InArgs, ULoomaSceneSyncSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;

    ChildSlot
    [
        SNew(SBorder)
        .Visibility(this, &SLoomaLoginWidget::GetRootVisibility)
        .Padding(12.0f)
        [
            SNew(SVerticalBox)

            // --- Signed in ---------------------------------------------------
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                .Visibility(this, &SLoomaLoginWidget::GetSignedInVisibility)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [
                    SNew(STextBlock).Text(this, &SLoomaLoginWidget::GetIdentityLabel)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("LogOut", "Log out"))
                    .OnClicked(this, &SLoomaLoginWidget::OnLogoutClicked)
                ]
            ]

            // --- Restored, not yet validated ---------------------------------
            //
            // Its own row rather than a variant of the signed-in one: we are showing a
            // name that came off disk and has not been confirmed by the backend, and
            // presenting that identically to a verified session would be a small lie.
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Visibility(this, &SLoomaLoginWidget::GetProvisionalVisibility)
                .Text(this, &SLoomaLoginWidget::GetIdentityLabel)
            ]

            // --- The form ----------------------------------------------------
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SVerticalBox)
                .Visibility(this, &SLoomaLoginWidget::GetFormVisibility)

                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [
                    SAssignNew(UsernameBox, SEditableTextBox)
                    .HintText(LOCTEXT("Username", "Username"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [
                    SAssignNew(PasswordBox, SEditableTextBox)
                    .HintText(LOCTEXT("Password", "Password"))
                    // Masked in the box AND never read anywhere but OnSubmitClicked,
                    // which hands it straight to RequestLogin. It is not stored on this
                    // widget and it is not logged: see the note in OnSubmitClicked.
                    .IsPassword(true)
                    // Enter submits. A login form that ignores the return key is the
                    // kind of thing nobody files a bug about and everybody resents.
                    .OnTextCommitted(this, &SLoomaLoginWidget::OnPasswordCommitted)
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SButton)
                    .Text(this, &SLoomaLoginWidget::GetSubmitLabel)
                    .IsEnabled(this, &SLoomaLoginWidget::IsSubmitEnabled)
                    .OnClicked(this, &SLoomaLoginWidget::OnSubmitClicked)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                [
                    SNew(STextBlock)
                    .Visibility(this, &SLoomaLoginWidget::GetErrorVisibility)
                    .Text(this, &SLoomaLoginWidget::GetErrorText)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.35f)))
                ]
            ]
        ]
    ];
}

EVisibility SLoomaLoginWidget::GetRootVisibility() const
{
    const ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    // Collapsed, not Hidden: an unknown or auth-disabled backend should take up no
    // layout space at all, not a blank rectangle where a form will maybe appear.
    if (!Sub || !Sub->IsAuthStateKnown() || !Sub->IsAuthEnabled())
    {
        return EVisibility::Collapsed;
    }
    return EVisibility::SelfHitTestInvisible;
}

EVisibility SLoomaLoginWidget::GetFormVisibility() const
{
    const ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    return (Sub && !Sub->HasAuthToken()) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SLoomaLoginWidget::GetProvisionalVisibility() const
{
    const ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    return (Sub && Sub->IsIdentityProvisional()) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SLoomaLoginWidget::GetSignedInVisibility() const
{
    const ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    return (Sub && Sub->HasAuthToken() && !Sub->IsIdentityProvisional())
        ? EVisibility::Visible
        : EVisibility::Collapsed;
}

FText SLoomaLoginWidget::GetIdentityLabel() const
{
    const ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    if (!Sub)
    {
        return FText::GetEmpty();
    }
    const FLoomaIdentity Identity = Sub->GetIdentity();
    if (Sub->IsIdentityProvisional())
    {
        return FText::Format(
            LOCTEXT("RestoredAs", "{0} — restored session, not yet validated"),
            FText::FromString(Identity.DisplayName));
    }
    return FText::Format(LOCTEXT("SignedInAs", "Signed in as {0}"),
        FText::FromString(Identity.DisplayName));
}

FText SLoomaLoginWidget::GetErrorText() const
{
    return ErrorText;
}

EVisibility SLoomaLoginWidget::GetErrorVisibility() const
{
    return ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

bool SLoomaLoginWidget::IsSubmitEnabled() const
{
    if (bLoginInFlight || !Subsystem.IsValid())
    {
        return false;
    }
    // Read straight off the boxes rather than mirroring their text into members: one
    // copy of the truth, and in particular no second copy of the password.
    const bool bHasUser = UsernameBox.IsValid() && !UsernameBox->GetText().IsEmpty();
    const bool bHasPass = PasswordBox.IsValid() && !PasswordBox->GetText().IsEmpty();
    return bHasUser && bHasPass;
}

FText SLoomaLoginWidget::GetSubmitLabel() const
{
    return bLoginInFlight
        ? LOCTEXT("SigningIn", "Signing in…")
        : LOCTEXT("LogIn", "Log in");
}

void SLoomaLoginWidget::OnPasswordCommitted(const FText& Text, ETextCommit::Type CommitType)
{
    if (CommitType == ETextCommit::OnEnter && IsSubmitEnabled())
    {
        OnSubmitClicked();
    }
}

FReply SLoomaLoginWidget::OnSubmitClicked()
{
    ULoomaSceneSyncSubsystem* Sub = Subsystem.Get();
    if (!Sub || bLoginInFlight)
    {
        return FReply::Handled();
    }

    bLoginInFlight = true;
    ErrorText = FText::GetEmpty();

    // Weak, so a login that outlives the widget — a level change, the viewport widget
    // removed mid-request — resolves into the subsystem exactly as it should and simply
    // has nobody left to tell. RequestLogin itself binds weakly to the subsystem, so if
    // that dies first the completion never runs at all; bLoginInFlight then stays set
    // and the button stays disabled, which is correct, because the game instance it
    // belonged to is going away with it.
    TWeakPtr<SLoomaLoginWidget> WeakSelf = SharedThis(this);

    // The password is read here and handed straight over. It is never stored on this
    // widget, never copied into a member, and never logged — not on the failure path
    // either. `ULoomaLoginAction` clears its own copy the moment the request owns one,
    // and this is the same discipline expressed by not making a copy at all.
    Sub->RequestLogin(UsernameBox->GetText().ToString(), PasswordBox->GetText().ToString(),
        [WeakSelf](bool bSuccess, const FLoomaIdentity& Identity, const FString& Error) {
            TSharedPtr<SLoomaLoginWidget> Self = WeakSelf.Pin();
            if (!Self.IsValid())
            {
                return;
            }
            Self->bLoginInFlight = false;
            if (bSuccess)
            {
                // Clear the password on success only. On failure it is almost always a
                // typo, and forcing a retype of something you cannot see is a worse
                // trade than the seconds the field lingers.
                if (Self->PasswordBox.IsValid())
                {
                    Self->PasswordBox->SetText(FText::GetEmpty());
                }
                Self->ErrorText = FText::GetEmpty();
                return;
            }
            // The subsystem's own wording, which is deliberately one generic message for
            // a wrong password and an unknown account alike — the backend's login
            // failures are non-enumerating on purpose and this must not undo that.
            Self->ErrorText = FText::FromString(Error);
        });

    return FReply::Handled();
}

FReply SLoomaLoginWidget::OnLogoutClicked()
{
    if (ULoomaSceneSyncSubsystem* Sub = Subsystem.Get())
    {
        Sub->Logout();
        ErrorText = FText::GetEmpty();
    }
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
