# GitHub Upload & Management Guide (ConsMLP)

This short guide explains how to upload the ConsMLP project to GitHub and manage it day‑to‑day.

## 1. Prerequisites
- Install Git and GitHub CLI (`gh`).
- Create a GitHub account and sign in:
  - `gh auth login`

## 2. Create a Local Git Repository
From the ConsMLP directory:
```
cd /home/xb1zhao/Desktop/workspace/ConsMLP
git init
git add .
git commit -m "Initial commit"
```

## 3. Create a GitHub Repository and Push
Option A: Create on GitHub website first, then:
```
git remote add origin https://github.com/<your-username>/ConsMLP.git
git branch -M main
git push -u origin main
```

Option B: Create via GitHub CLI:
```
gh repo create ConsMLP --public --source . --remote origin --push
```

## 4. Typical Daily Workflow
```
git status
git add <files>
git commit -m "Describe the change"
git push
```

## 5. Work with Branches (Recommended)
Create a feature branch:
```
git checkout -b feature/my-change
```
Push the branch:
```
git push -u origin feature/my-change
```
Open a pull request:
```
gh pr create --title "My change" --body "What and why"
```

## 6. Sync with Remote Changes
```
git fetch
git pull
```

## 7. Helpful Commands
- View history: `git log --oneline`
- View differences: `git diff`
- Discard local changes (careful): `git restore <file>`

## 8. Recommended .gitignore
If you do not have one, create `.gitignore` for build output and temp files. Example:
```
build/
*.log
*.tmp
```

## 9. Release Tags (Optional)
```
git tag -a v1.0.0 -m "First release"
git push origin v1.0.0
```

---
If you want, tell me your preferred repository name and visibility (public/private), and I can tailor the exact commands.
