#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Enhanced version with Windows administrator user support and SSH service restart
"""

import argparse, json, os, posixpath, socket, sys, traceback, subprocess, stat
import paramiko

def _normalize_path(p):
    if p is None: return None
    p = str(p).strip().strip('"').strip("'")
    p = os.path.expandvars(p)
    p = os.path.expanduser(p)
    return os.path.normpath(p)

def _known_hosts_path() -> str:
    return _normalize_path("~/.ssh/known_hosts")

def _known_host_label(host: str, port: int) -> str:
    needs_bracket = (":" in host)
    if port == 22 and not needs_bracket:
        return host
    return f"[{host}]:{port}"

def _ensure_known_hosts_entry(host: str, port: int, pkey) -> None:
    path = _known_hosts_path()
    ssh_dir = os.path.dirname(path)
    os.makedirs(ssh_dir, exist_ok=True)
    label = _known_host_label(host, port)
    line = f"{label} {pkey.get_name()} {pkey.get_base64()}"
    try:
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
    except FileNotFoundError:
        content = ""

    if line not in content:
        with open(path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if os.name == "posix":
            try: os.chmod(ssh_dir, 0o700)
            except Exception: pass
            try: os.chmod(path, 0o644)
            except Exception: pass
        print(f"[LOCAL] known_hosts 등록: {label} ({pkey.get_name()})")
    else:
        print(f"[LOCAL] known_hosts 이미 등록됨: {label}")

# ---------------- GUI 유틸 ----------------
'''
def ask_password_with_visibility_option(title: str, prompt: str):
    """GUI가 가능한 경우 비밀번호 보기 옵션과 함께 입력받기"""
    try:
        import tkinter as tk
        from tkinter import simpledialog, messagebox, BooleanVar, Checkbutton, Entry, Label, Button, Frame
        
        class PasswordDialog:
            def __init__(self, parent, title, prompt):
                self.result = None
                
                # 다이얼로그 창 생성
                self.dialog = tk.Toplevel(parent)
                self.dialog.title(title)
                self.dialog.geometry("400x150")
                self.dialog.resizable(False, False)
                self.dialog.transient(parent)
                self.dialog.grab_set()
                
                # 창을 화면 중앙에 배치
                self.dialog.update_idletasks()
                x = (self.dialog.winfo_screenwidth() // 2) - (400 // 2)
                y = (self.dialog.winfo_screenheight() // 2) - (150 // 2)
                self.dialog.geometry(f"400x150+{x}+{y}")
                
                # 프롬프트 라벨
                Label(self.dialog, text=prompt, font=("Arial", 10)).pack(pady=(10, 5))
                
                # 비밀번호 입력 프레임
                input_frame = Frame(self.dialog)
                input_frame.pack(pady=5, padx=20, fill='x')
                
                # 비밀번호 입력 필드
                self.password_var = tk.StringVar()
                self.show_password = BooleanVar()
                self.entry = Entry(input_frame, textvariable=self.password_var, 
                                 show='*', font=("Arial", 10), width=40)
                self.entry.pack(fill='x')
                
                # 비밀번호 보기 체크박스
                check_frame = Frame(self.dialog)
                check_frame.pack(pady=5)
                self.show_checkbox = Checkbutton(check_frame, text="비밀번호 보기", 
                                               variable=self.show_password,
                                               command=self.toggle_password_visibility,
                                               font=("Arial", 9))
                self.show_checkbox.pack()
                
                # 버튼 프레임
                button_frame = Frame(self.dialog)
                button_frame.pack(pady=10)
                
                Button(button_frame, text="확인", command=self.ok_clicked, 
                      width=8, font=("Arial", 9)).pack(side='left', padx=5)
                Button(button_frame, text="취소", command=self.cancel_clicked, 
                      width=8, font=("Arial", 9)).pack(side='left', padx=5)
                
                # 엔터 키로 확인
                self.entry.bind('<Return>', lambda e: self.ok_clicked())
                self.entry.bind('<Escape>', lambda e: self.cancel_clicked())
                
                # 포커스 설정
                self.entry.focus_set()
                
            def toggle_password_visibility(self):
                if self.show_password.get():
                    self.entry.config(show='')
                else:
                    self.entry.config(show='*')
                    
            def ok_clicked(self):
                self.result = self.password_var.get()
                self.dialog.destroy()
                
            def cancel_clicked(self):
                self.result = None
                self.dialog.destroy()
        
        root = tk.Tk()
        root.withdraw()
        
        try:
            dialog = PasswordDialog(root, title, prompt)
            root.wait_window(dialog.dialog)
            return dialog.result
        finally:
            try: 
                root.destroy()
            except Exception: 
                pass
                
    except Exception as e:
        print(f"[DEBUG] GUI 비밀번호 입력 실패, 콘솔로 전환: {e}")
        # GUI 실패 시 콘솔 입력으로 폴백
        import getpass
        try:
            # 콘솔에서도 보기 옵션 제공
            show_option = input(f"{title} - 비밀번호를 화면에 표시하시겠습니까? (y/N): ").lower().strip()
            
            if show_option in ['y', 'yes', '예']:
                pwd = input(f"{title} - {prompt} (표시됨): ")
            else:
                pwd = getpass.getpass(f"{title} - {prompt} (숨김): ")
            return pwd if pwd else None
        except (EOFError, KeyboardInterrupt):
            return None
            
def ask_password_masked(title: str, prompt: str):
    """기존 함수 호환성을 위해 유지 (새 함수 호출)"""
    return ask_password_with_visibility_option(title, prompt)1
'''

def ask_password_masked(title: str, prompt: str):
    try:
        import tkinter as tk
        from tkinter import simpledialog
        root = tk.Tk(); root.withdraw()
        try:
            pwd = simpledialog.askstring(title, prompt, show='*')
        finally:
            try: root.destroy()
            except Exception: pass
        return pwd
    except Exception:
        import getpass
        try:
            pwd = getpass.getpass(f"{title} - {prompt}: ")
            return pwd if pwd else None
        except (EOFError, KeyboardInterrupt):
            return None

def show_error_dialog(title: str, message: str):
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk(); root.withdraw()
        try: messagebox.showerror(title, message)
        finally:
            try: root.destroy()
            except Exception: pass
    except Exception:
        print(f"[ERROR] {title}: {message}")

def ask_retry_cancel(title: str, message: str) -> bool:
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk(); root.withdraw()
        try: return messagebox.askretrycancel(title, message)
        finally:
            try: root.destroy()
            except Exception: pass
    except Exception:
        print(f"[WARN ] {title}: {message} (no GUI; not retrying)")
        return False

# --------------- 예외 ---------------
class PasswordAuthError(Exception): ...
class SshConnectError(Exception): ...

# --------------- 키/플랫폼/권한 유틸 ---------------
def _load_private_key(priv_path, passphrase=None):
    exc = None
    for KeyCls in (paramiko.Ed25519Key, paramiko.RSAKey, paramiko.ECDSAKey):
        try:
            return KeyCls.from_private_key_file(priv_path, password=passphrase)
        except Exception as e:
            exc = e
    raise exc

def _detect_remote_platform(ssh):
    try:
        _stdin, _stdout, _stderr = ssh.exec_command(
            'powershell -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.Major"', timeout=10)
        if _stdout.channel.recv_exit_status() == 0:
            return "windows"
    except Exception:
        pass
    try:
        _stdin, _stdout, _stderr = ssh.exec_command("uname", timeout=10)
        if _stdout.channel.recv_exit_status() == 0:
            return "posix"
    except Exception:
        pass
    return "windows"

def _get_remote_home_dir(ssh, platform):
    if platform == "windows":
        commands = [
            'powershell -NoProfile -NonInteractive -Command "$env:USERPROFILE"',
            'echo %USERPROFILE%',
            'powershell -NoProfile -NonInteractive -Command "(Get-Location).Path"'
        ]
        
        for cmd in commands:
            try:
                _stdin, _stdout, _stderr = ssh.exec_command(cmd, timeout=10)
                home = _stdout.read().decode().strip()
                if home and not home.startswith('$') and os.path.isabs(home.replace('\\', '/')):
                    print(f"[DEBUG] Windows 홈 디렉터리 감지: {home} (명령: {cmd})")
                    return home
            except Exception as e:
                print(f"[DEBUG] 명령 실패 '{cmd}': {e}")
                continue
        
        _stdin, _stdout, _stderr = ssh.exec_command('whoami', timeout=10)
        username = _stdout.read().decode().strip()
        if username and '\\' in username:
            username = username.split('\\')[-1]
        if username:
            fallback_home = f"C:\\Users\\{username}"
            print(f"[DEBUG] Windows 홈 디렉터리 폴백: {fallback_home}")
            return fallback_home
    else:
        commands = [
            'echo "$HOME"',
            'pwd',
            'echo ~'
        ]
        
        for cmd in commands:
            try:
                _stdin, _stdout, _stderr = ssh.exec_command(cmd, timeout=10)
                home = _stdout.read().decode().strip()
                if home and not home.startswith('$') and home.startswith('/'):
                    print(f"[DEBUG] POSIX 홈 디렉터리 감지: {home} (명령: {cmd})")
                    return home
            except Exception as e:
                print(f"[DEBUG] 명령 실패 '{cmd}': {e}")
                continue
    
    raise RuntimeError(f"원격 홈 디렉터리를 확인할 수 없습니다 (플랫폼: {platform})")

def _check_if_windows_admin(ssh, username):
    """Check if the user is a Windows administrator"""
    try:
        cmd = f'net localgroup administrators | findstr /i "{username}"'
        _stdin, _stdout, _stderr = ssh.exec_command(cmd, timeout=10)
        exit_code = _stdout.channel.recv_exit_status()
        output = _stdout.read().decode(errors="replace").strip()
        
        print(f"[DEBUG] 관리자 확인 (exit: {exit_code}): {output}")
        
        # If exit code is 0 and username appears in output, user is admin
        is_admin = (exit_code == 0 and username.lower() in output.lower())
        print(f"[DEBUG] 사용자 '{username}' 관리자 여부: {is_admin}")
        return is_admin
        
    except Exception as e:
        print(f"[DEBUG] 관리자 확인 실패: {e}")
        return False

def _restart_ssh_service(ssh, platform):
    """Restart SSH service to reload configuration"""
    if platform == "windows":
        try:
            print("[INFO] SSH 서비스 재시작 중...")
            cmd = 'powershell -NoProfile -NonInteractive -Command "Restart-Service sshd -Force"'
            _stdin, _stdout, _stderr = ssh.exec_command(cmd, timeout=30)
            exit_code = _stdout.channel.recv_exit_status()
            
            if exit_code == 0:
                print("[INFO] SSH 서비스 재시작 성공")
                return True
            else:
                err_output = _stderr.read().decode(errors="replace").strip()
                print(f"[WARN] SSH 서비스 재시작 실패 (exit: {exit_code}): {err_output}")
                return False
                
        except Exception as e:
            print(f"[WARN] SSH 서비스 재시작 중 오류: {e}")
            return False
    else:
        try:
            print("[INFO] SSH 서비스 재시작 중...")
            # Try common Linux SSH service restart commands
            for cmd in ["sudo systemctl restart sshd", "sudo service ssh restart", "sudo /etc/init.d/ssh restart"]:
                try:
                    _stdin, _stdout, _stderr = ssh.exec_command(cmd, timeout=30)
                    exit_code = _stdout.channel.recv_exit_status()
                    if exit_code == 0:
                        print("[INFO] SSH 서비스 재시작 성공")
                        return True
                except Exception:
                    continue
            
            print("[WARN] SSH 서비스 재시작 실패 (sudo 권한 필요할 수 있음)")
            return False
            
        except Exception as e:
            print(f"[WARN] SSH 서비스 재시작 중 오류: {e}")
            return False

def _copy_to_admin_authorized_keys(ssh, user_auth_keys_path, username):
    """Copy authorized_keys to admin location for Windows admin users"""
    admin_auth_keys = "C:/ProgramData/ssh/administrators_authorized_keys"
    
    try:
        # Fix SFTP path format: /C:/Users/... -> C:/Users/...
        if user_auth_keys_path.startswith("/C:/"):
            user_path_corrected = user_auth_keys_path[1:]  # Remove leading /
        else:
            user_path_corrected = user_auth_keys_path
        
        # Convert paths for Windows commands (use backslashes)
        user_path_win = user_path_corrected.replace("/", "\\")
        admin_path_win = admin_auth_keys.replace("/", "\\")
        
        print(f"[INFO] 관리자 authorized_keys로 복사: {admin_path_win}")
        print(f"[DEBUG] 원본 경로: {user_path_win}")
        
        # Create ProgramData/ssh directory if it doesn't exist
        cmd_mkdir = f'powershell -NoProfile -NonInteractive -Command "New-Item -ItemType Directory -Path C:\\ProgramData\\ssh -Force"'
        ssh.exec_command(cmd_mkdir, timeout=15)
        
        # Copy the file using proper Windows path
        cmd_copy = f'powershell -NoProfile -NonInteractive -Command "Copy-Item \\"{user_path_win}\\" \\"{admin_path_win}\\" -Force"'
        print(f"[DEBUG] 복사 명령: {cmd_copy}")
        
        _stdin, _stdout, _stderr = ssh.exec_command(cmd_copy, timeout=15)
        exit_code = _stdout.channel.recv_exit_status()
        
        if exit_code == 0:
            print("[INFO] 관리자 authorized_keys 복사 성공")
            
            # Set proper permissions for admin authorized_keys
            cmd_perm = f'powershell -NoProfile -NonInteractive -Command "icacls \\"{admin_path_win}\\" /inheritance:r /grant:r \\"Administrators:(R,W)\\" /grant:r \\"SYSTEM:(R,W)\\""'
            ssh.exec_command(cmd_perm, timeout=15)
            
            return True
        else:
            err_output = _stderr.read().decode(errors="replace").strip()
            out_output = _stdout.read().decode(errors="replace").strip()
            print(f"[WARN] 관리자 authorized_keys 복사 실패 (exit: {exit_code})")
            if out_output:
                print(f"[DEBUG] stdout: {out_output}")
            if err_output:
                print(f"[DEBUG] stderr: {err_output}")
            return False
            
    except Exception as e:
        print(f"[WARN] 관리자 authorized_keys 복사 중 오류: {e}")
        return False

def _ensure_ssh_dir_and_perms(ssh, platform, home_dir_shell, home_dir_sftp: str | None = None):
    if home_dir_sftp is None:
        home_dir_sftp = home_dir_shell.replace("\\", "/")

    if platform == "posix":
        ssh_dir_shell = posixpath.join(home_dir_shell, ".ssh")
        auth_keys_shell = posixpath.join(ssh_dir_shell, "authorized_keys")

        ssh.exec_command(f"mkdir -p '{ssh_dir_shell}' && chmod 700 '{ssh_dir_shell}'")
        ssh.exec_command(f"touch '{auth_keys_shell}' && chmod 600 '{auth_keys_shell}'")

        ssh_dir_sftp = posixpath.join(home_dir_sftp, ".ssh")
        auth_keys_sftp = posixpath.join(ssh_dir_sftp, "authorized_keys")
    else:
        ssh_dir_shell = f"{home_dir_shell}\\.ssh"
        auth_keys_shell = f"{ssh_dir_shell}\\authorized_keys"

        ps = rf"""
$ErrorActionPreference='Stop'
$ssh='{ssh_dir_shell}'
$auth='{auth_keys_shell}'

if (!(Test-Path $ssh)) {{ 
    New-Item -ItemType Directory -Path $ssh -Force | Out-Null 
    Write-Output "Created .ssh directory"
}}

if (!(Test-Path $auth)) {{ 
    New-Item -ItemType File -Path $auth -Force | Out-Null 
    Write-Output "Created authorized_keys file"
}}

$currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
Write-Output "Setting permissions for user: $currentUser"

try {{
    icacls $ssh /inheritance:r /grant:r "$currentUser:(OI)(CI)(F)" /T | Out-Null
    Write-Output "Set .ssh directory permissions"
    
    icacls $auth /inheritance:r /grant:r "$currentUser:(R,W)" | Out-Null
    Write-Output "Set authorized_keys file permissions"
    
    icacls $ssh /grant:r "SYSTEM:(OI)(CI)(F)" /T | Out-Null
    icacls $auth /grant:r "SYSTEM:(R,W)" | Out-Null
    Write-Output "Set SYSTEM permissions"
    
}} catch {{
    Write-Output "Permission setting failed: $_"
}}
"""
        _stdin, _stdout, _stderr = ssh.exec_command(f"powershell -NoProfile -NonInteractive -Command \"{ps}\"", timeout=30)
        exit_code = _stdout.channel.recv_exit_status()
        out = _stdout.read().decode(errors="replace").strip()
        err = _stderr.read().decode(errors="replace").strip()
        
        print(f"[DEBUG] Windows 권한 설정 결과 (exit: {exit_code})")
        if out:
            print(f"[DEBUG] stdout: {out}")
        if err:
            print(f"[DEBUG] stderr: {err}")

        ssh_dir_sftp = posixpath.join(home_dir_sftp, ".ssh")
        auth_keys_sftp = posixpath.join(ssh_dir_sftp, "authorized_keys")

    return ssh_dir_shell, auth_keys_shell, ssh_dir_sftp, auth_keys_sftp

def _append_key_if_missing(ssh, sftp, auth_keys_sftp, public_key_text):
    existing = ""
    try:
        with sftp.open(auth_keys_sftp, "r") as f:
            existing = f.read().decode(errors="replace")
    except IOError:
        existing = ""
    
    new_key_parts = public_key_text.strip().split()
    if len(new_key_parts) >= 2:
        new_key_core = f"{new_key_parts[0]} {new_key_parts[1]}"
        
        for line in existing.splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            existing_parts = line.split()
            if len(existing_parts) >= 2:
                existing_key_core = f"{existing_parts[0]} {existing_parts[1]}"
                if new_key_core == existing_key_core:
                    print(f"[DEBUG] 키가 이미 존재함: {new_key_core[:50]}...")
                    return False
    
    parent = posixpath.dirname(auth_keys_sftp.rstrip("/"))
    try:
        sftp.stat(parent)
    except IOError:
        sftp.mkdir(parent)
    
    with sftp.open(auth_keys_sftp, "a") as f:
        f.write(public_key_text.strip() + "\n")
    
    print(f"[DEBUG] 새 키 추가됨: {new_key_parts[0] if new_key_parts else 'unknown'}...")
    return True

# -------- 로컬 키쌍 보장(없으면 생성) --------
def _posix() -> bool:
    return os.name == "posix"

def _ensure_dir_and_perms(path_dir: str):
    os.makedirs(path_dir, exist_ok=True)
    if _posix():
        os.chmod(path_dir, 0o700)

def _fix_key_permissions(priv_path: str, pub_path: str):
    if _posix():
        try: os.chmod(priv_path, 0o600)
        except Exception: pass
        try: os.chmod(pub_path, 0o644)
        except Exception: pass

def _default_comment():
    user = os.environ.get("USER") or os.environ.get("USERNAME") or "user"
    host = socket.gethostname()
    return f"{user}@{host}"

def _ensure_local_keypair(pub_path: str, key_type: str = "ed25519", passphrase: str | None = None, comment: str | None = None):
    pub_path = os.path.expanduser(pub_path)
    if not pub_path.endswith(".pub"):
        raise ValueError("public key 경로는 .pub 확장자를 포함해야 합니다.")
    priv_path = pub_path[:-4]

    key_dir = os.path.dirname(priv_path) or os.path.expanduser("~/.ssh")
    _ensure_dir_and_perms(key_dir)

    priv_exists = os.path.exists(priv_path)
    pub_exists  = os.path.exists(pub_path)

    if priv_exists and pub_exists:
        _fix_key_permissions(priv_path, pub_path)
        return priv_path, pub_path, "none"

    if priv_exists and not pub_exists:
        try:
            res = subprocess.run(
                ["ssh-keygen", "-y", "-f", priv_path],
                capture_output=True, text=True, check=False
            )
            if res.returncode == 0 and res.stdout.strip():
                with open(pub_path, "w", encoding="utf-8") as f:
                    line = res.stdout.strip()
                    if comment := (comment or _default_comment()):
                        if " " in line:
                            parts = line.split()
                            if len(parts) >= 2:
                                line = f"{parts[0]} {parts[1]} {comment}"
                    f.write(line + "\n")
                _fix_key_permissions(priv_path, pub_path)
                return priv_path, pub_path, "pub_created"
        except FileNotFoundError:
            pass

        try:
            key = _load_private_key(priv_path, passphrase=None)
            line = f"{key.get_name()} {key.get_base64()} {(comment or _default_comment())}"
            with open(pub_path, "w", encoding="utf-8") as f:
                f.write(line + "\n")
            _fix_key_permissions(priv_path, pub_path)
            return priv_path, pub_path, "pub_created"
        except Exception as e:
            raise RuntimeError(f"기존 개인키에서 공개키 생성 실패: {e}")

    try:
        args = ["ssh-keygen", "-t", key_type, "-f", priv_path, "-N", passphrase or "", "-C", comment or _default_comment()]
        res = subprocess.run(args, capture_output=True, text=True, check=False)
        if res.returncode == 0 and os.path.exists(priv_path) and os.path.exists(pub_path):
            _fix_key_permissions(priv_path, pub_path)
            return priv_path, pub_path, "pair_created"
    except FileNotFoundError:
        pass

    key = paramiko.RSAKey.generate(3072)
    key.write_private_key_file(priv_path)
    line = f"{key.get_name()} {key.get_base64()} {(comment or _default_comment())}"
    with open(pub_path, "w", encoding="utf-8") as f:
        f.write(line + "\n")
    _fix_key_permissions(priv_path, pub_path)
    return priv_path, pub_path, "pair_created"

# --------------- 배포/검증 ---------------
def deploy_key_with_password(host, port, username, password, pubkey_path):
    priv_path, pub_path, created = _ensure_local_keypair(pubkey_path)
    if created != "none":
        print(f"[LOCAL] 키 생성: {created} (priv: {priv_path}, pub: {pub_path})")

    with open(pub_path, "r", encoding="utf-8") as f:
        public_key_text = f.read().strip()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        try:
            ssh.connect(host, port=port, username=username, password=password,
                        look_for_keys=False, allow_agent=False,
                        timeout=15, banner_timeout=15, auth_timeout=15)
            
            server_key = ssh.get_transport().get_remote_server_key()
            _ensure_known_hosts_entry(host, port, server_key)

        except paramiko.AuthenticationException as e:
            raise PasswordAuthError("비밀번호가 올바르지 않습니다.") from e
        except (paramiko.SSHException, socket.error) as e:
            raise SshConnectError(f"SSH 연결 실패: {e}") from e

        platform = _detect_remote_platform(ssh)
        print(f"[DEBUG] 감지된 플랫폼: {platform}")

        home_dir_shell = _get_remote_home_dir(ssh, platform)

        # Check if user is Windows admin
        is_windows_admin = False
        if platform == "windows":
            is_windows_admin = _check_if_windows_admin(ssh, username)

        sftp = ssh.open_sftp()
        try:
            sftp_home = sftp.normalize(".")
            print(f"[DEBUG] SFTP 홈: {sftp_home}")
            
            ssh_dir_shell, auth_keys_shell, ssh_dir_sftp, auth_keys_sftp = _ensure_ssh_dir_and_perms(
                ssh, platform, home_dir_shell, sftp_home
            )

            changed = _append_key_if_missing(ssh, sftp, auth_keys_sftp, public_key_text)
        finally:
            try: sftp.close()
            except Exception: pass

        if platform == "posix":
            ssh.exec_command(f"chmod 700 '{ssh_dir_shell}' && chmod 600 '{auth_keys_shell}'")

        # Windows admin user special handling
        admin_key_copied = False
        if platform == "windows" and is_windows_admin:
            admin_key_copied = _copy_to_admin_authorized_keys(ssh, auth_keys_sftp, username)

        # Restart SSH service
        service_restarted = _restart_ssh_service(ssh, platform)

        return {
            "platform": platform, 
            "home_shell": home_dir_shell, 
            "home_sftp": sftp_home,
            "ssh_dir": ssh_dir_sftp, 
            "auth_keys": auth_keys_sftp, 
            "changed": changed, 
            "priv_path": priv_path,
            "is_admin": is_windows_admin,
            "admin_key_copied": admin_key_copied,
            "service_restarted": service_restarted
        }
    finally:
        try: ssh.close()
        except Exception: pass

def test_key_login(host, port, username, privkey_path, passphrase=None):
    privkey_path = os.path.expanduser(privkey_path)
    if not os.path.exists(privkey_path):
        raise FileNotFoundError(f"개인키 파일을 찾을 수 없습니다: {privkey_path}")
    
    try:
        pkey = _load_private_key(privkey_path, passphrase)
        print(f"[DEBUG] 로드된 키 타입: {type(pkey).__name__}")
    except Exception as e:
        raise RuntimeError(f"개인키 로드 실패: {e}")

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        print(f"[DEBUG] 키 인증 시도: {username}@{host}:{port}")
        ssh.connect(host, port=port, username=username, pkey=pkey,
                    look_for_keys=False, allow_agent=False,
                    timeout=15, banner_timeout=15, auth_timeout=15)
        
        server_key = ssh.get_transport().get_remote_server_key()
        _ensure_known_hosts_entry(host, port, server_key)

        platform = _detect_remote_platform(ssh)
        cmd = "echo OK:$(whoami) on $(hostname)" if platform == "posix" else \
              ("powershell -NoProfile -NonInteractive -Command "
               "\"Write-Output ('OK:'+ $env:USERNAME +' on '+ $env:COMPUTERNAME)\"")
        _stdin, _stdout, _stderr = ssh.exec_command(cmd)
        exit_code = _stdout.channel.recv_exit_status()
        out = _stdout.read().decode(errors="replace").strip()
        err = _stderr.read().decode(errors="replace").strip()
        
        print(f"[DEBUG] 테스트 명령 결과 (exit: {exit_code}): {out}")
        if err:
            print(f"[DEBUG] 테스트 명령 stderr: {err}")
            
        return {"exit": exit_code, "stdout": out, "stderr": err, "platform": platform}
    finally:
        try: ssh.close()
        except Exception: pass

# --------------- 설정 로드/경로 결정 ---------------
def _load_config(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)

def _resolve_pub_priv_paths(cfg, srv):
    pub = srv.get("pubkey") or cfg.get("default_pubkey_path") or cfg.get("default_key_path")
    if not pub:
        pub = "~/.ssh/id_ed25519.pub"
    pub = os.path.expanduser(pub)

    priv = srv.get("privkey") or cfg.get("default_privkey_path")
    if not priv and pub.endswith(".pub"):
        priv = pub[:-4]
    priv = os.path.expanduser(priv) if priv else None
    
    print(f"[DEBUG] 키 경로 결정 - pub: {pub}, priv: {priv}")
    return pub, priv

# --------------- 메인 ---------------
def main():
    ap = argparse.ArgumentParser(description="Deploy SSH public keys using JSON; prompt password via GUI when needed.")
    ap.add_argument("--config", default="workers_list.json", help="JSON 설정 파일 경로 (기본: workers_list.json)")
    args = ap.parse_args()

    try:
        cfg = _load_config(args.config)
    except Exception as e:
        print(f"[FATAL] JSON 설정 로드 실패: {e}")
        sys.exit(2)

    nodes = cfg.get("workers", None)
    if nodes is None:
        nodes = cfg.get("servers", [])
    if not isinstance(nodes, list) or not nodes:
        print("[FATAL] 'workers' 배열이 비어있거나 잘못되었습니다. (구버전 'servers'도 없음)")
        sys.exit(2)

    successes, failures, skipped = [], [], []

    for i, srv in enumerate(nodes, 1):
        host = srv.get("host")
        port = int(srv.get("port", 22))
        username = srv.get("username")
        if not all([host, username]):
            print(f"[{i:02d}] 구성 누락 → host/username 필요")
            failures.append((f"{host}:{port}", "구성 누락"))
            continue

        try:
            pubkey_path, privkey_path = _resolve_pub_priv_paths(cfg, srv)
        except Exception as e:
            print(f"[{i:02d}] {host}:{port} 구성 오류 → {e}")
            failures.append((f"{host}:{port}", f"구성 오류: {e}"))
            continue

        print(f"\n[CHECK {i:02d}] 키 인증 접속 확인 → {username}@{host}:{port}")
        key_ok = False
        if privkey_path and os.path.exists(privkey_path):
            try:
                r = test_key_login(host, port, username, privkey_path)
                if r["exit"] == 0 and r["stdout"].startswith("OK:"):
                    print("[OK   ] 이미 키 인증 접속이 가능합니다. 배포 불필요.")
                    successes.append((f"{host}:{port}", "이미 배포됨(무비번 OK)"))
                    key_ok = True
                else:
                    print("[WARN ] 접속됨이나 기대 출력과 다름 → 배포 시도")
            except Exception as e:
                print(f"[INFO ] 현재는 키 인증 접속 불가 → 배포 시도 ({e})")
        else:
            print("[INFO ] 개인키가 없거나 경로 미지정 → 배포 시도(필요 시 자동 생성)")

        if key_ok:
            continue

        # -------- 비밀번호 입력 → 배포 루프 --------
        while True:
            password = srv.get("password")
            if not password:
                password = ask_password_masked("SSH Password Required",
                                               f"Enter password for {username}@{host}:{port}")
            if not password:
                print(f"[SKIP ] 사용자 취소 → {host}:{port} 배포 건너뜀")
                skipped.append((f"{host}:{port}", "사용자 취소"))
                break

            print(f"[STEP 1] 공개키 배포 시도 → {username}@{host}:{port}")
            try:
                info = deploy_key_with_password(host, port, username, password, pubkey_path)
                print(f"[INFO ] 플랫폼: {info['platform']} / 홈(shell): {info['home_shell']} / 홈(sftp): {info['home_sftp']}")
                print(f"[INFO ] SSH 디렉터리(SFTP): {info['ssh_dir']}")
                print(f"[INFO ] authorized_keys(SFTP): {info['auth_keys']}")
                print(f"[INFO ] 키 {'추가됨' if info['changed'] else '이미 존재'}")
                
                # Windows 관리자 사용자 특별 처리 결과 출력
                if info.get('is_admin'):
                    print(f"[INFO ] Windows 관리자 사용자 감지됨")
                    if info.get('admin_key_copied'):
                        print(f"[INFO ] 관리자 authorized_keys 복사 성공")
                    else:
                        print(f"[WARN ] 관리자 authorized_keys 복사 실패")
                
                # SSH 서비스 재시작 결과 출력
                if info.get('service_restarted'):
                    print(f"[INFO ] SSH 서비스 재시작 성공")
                else:
                    print(f"[WARN ] SSH 서비스 재시작 실패 또는 생략")
                
                if not privkey_path:
                    privkey_path = info.get("priv_path")
                    
            except PasswordAuthError:
                show_error_dialog("Authentication Failed", "비밀번호가 올바르지 않습니다. 다시 시도하세요.")
                continue
            except SshConnectError as e:
                if ask_retry_cancel("SSH 연결 실패", f"{e}\n재시도하시겠습니까?"):
                    continue
                failures.append((f"{host}:{port}", f"연결 실패: {e}"))
                break
            except FileNotFoundError as e:
                show_error_dialog("키 파일 없음", str(e))
                failures.append((f"{host}:{port}", f"키 파일 없음: {e}"))
                break
            except Exception as e:
                msg = f"예상치 못한 오류: {e}\n{traceback.format_exc(limit=1)}"
                if ask_retry_cancel("배포 오류", msg + "\n재시도하시겠습니까?"):
                    continue
                failures.append((f"{host}:{port}", f"배포 오류: {e}"))
                break

            # 배포 성공 → 키 인증 재테스트
            if not privkey_path or not os.path.exists(privkey_path):
                note = "배포됨(개인키 파일 부재로 테스트 불가)"
                print(f"[WARN ] {note}")
                successes.append((f"{host}:{port}", note))
                break

            print(f"[STEP 2] 키 인증 재접속 테스트 → {privkey_path}")
            try:
                # 잠시 대기 후 테스트 (SSH 서비스 재시작 반영 시간)
                import time
                time.sleep(2)
                
                r = test_key_login(host, port, username, privkey_path)
                if r["exit"] == 0 and r["stdout"].startswith("OK:"):
                    print("[OK   ] 키 인증 접속 성공 (암호 프롬프트 없이 접속됨).")
                    successes.append((f"{host}:{port}", "배포됨(무비번 OK)"))
                else:
                    print("[WARN ] 접속은 되었으나 기대 출력과 다름")
                    successes.append((f"{host}:{port}", "배포됨(검증 경고)"))
            except Exception as e:
                print(f"[ERROR] 키 인증 테스트 실패 → {e}")
                failures.append((f"{host}:{port}", f"검증 실패: {e}"))
            break

    # ---------- 요약 ----------
    print("\n========== 배포 요약 ==========")
    if successes:
        print(f"[성공] {len(successes)}대")
        [print(f"  - {h} : {note}") for h, note in successes]
    else:
        print("[성공] 0대")

    if skipped:
        print(f"[건너뜀] {len(skipped)}대")
        [print(f"  - {h} : {note}") for h, note in skipped]
    else:
        print("[건너뜀] 0대")

    if failures:
        print(f"[실패] {len(failures)}대")
        [print(f"  - {h} : {reason}") for h, reason in failures]
    else:
        print("[실패] 0대")
    print("================================")

    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()