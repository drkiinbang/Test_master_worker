#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import posixpath
import socket
import paramiko

def _load_private_key(priv_path, passphrase=None):
    """개인키 형식을 자동 추론(Ed25519 → RSA → ECDSA 순)하여 로드."""
    exc = None
    for KeyCls in (paramiko.Ed25519Key, paramiko.RSAKey, paramiko.ECDSAKey):
        try:
            return KeyCls.from_private_key_file(priv_path, password=passphrase)
        except Exception as e:
            exc = e
    raise exc

def _detect_remote_platform(ssh):
    """원격 플랫폼 감지: windows / posix"""
    try:
        _stdin, _stdout, _stderr = ssh.exec_command(
            "powershell -NoProfile -NonInteractive -Command \"$PSVersionTable.PSVersion.Major\"",
            timeout=10,
        )
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
    # 기본값
    return "windows"

def _ensure_ssh_dir_and_perms(ssh, platform, home_dir):
    """~/.ssh 디렉터리와 권한 설정 보정."""
    ssh_dir = posixpath.join(home_dir, ".ssh")
    auth_keys = posixpath.join(ssh_dir, "authorized_keys")

    if platform == "posix":
        # 디렉터리 및 권한
        ssh.exec_command(f"mkdir -p '{ssh_dir}' && chmod 700 '{ssh_dir}'")
        # 파일 없으면 생성
        ssh.exec_command(f"touch '{auth_keys}' && chmod 600 '{auth_keys}'")
    else:
        # Windows(OpenSSH) 권한: NTFS ACL로 제한 필요
        ps = rf"""
$ErrorActionPreference='Stop'
$ssh='{ssh_dir}'
$auth='{auth_keys}'
if (!(Test-Path $ssh)) {{ New-Item -ItemType Directory -Path $ssh | Out-Null }}
if (!(Test-Path $auth)) {{ New-Item -ItemType File -Path $auth | Out-Null }}
# 상속 제거 및 사용자 전용 부여
$u = "$env:USERNAME"
icacls $ssh /inheritance:r | Out-Null
icacls $ssh /grant:r $u:(OI)(CI)(F) /T | Out-Null
icacls $auth /inheritance:r | Out-Null
icacls $auth /grant:r $u:(R,W) | Out-Null
"""
        ssh.exec_command(f"powershell -NoProfile -NonInteractive -Command \"{ps}\"")

    return ssh_dir, auth_keys

def _append_key_if_missing(ssh, sftp, auth_keys, public_key_text):
    """authorized_keys에 공개키가 없으면 추가."""
    try:
        existing = ""
        try:
            with sftp.open(auth_keys, "r") as f:
                existing = f.read().decode(errors="replace")
        except IOError:
            existing = ""
        if public_key_text.strip() not in existing:
            with sftp.open(auth_keys, "a") as f:
                f.write(public_key_text.strip() + "\n")
            return True
        return False
    finally:
        pass

def deploy_key(host, port, username, password, pubkey_path):
    """1) 비밀번호 인증으로 접속 → 2) 키 등록 → 3) 권한 보정"""
    if not os.path.exists(os.path.expanduser(pubkey_path)):
        raise FileNotFoundError(f"공개키 파일을 찾을 수 없습니다: {pubkey_path}")
    with open(os.path.expanduser(pubkey_path), "r", encoding="utf-8") as f:
        public_key_text = f.read().strip()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(
            host, port=port, username=username, password=password,
            look_for_keys=False, allow_agent=False, timeout=15, banner_timeout=15, auth_timeout=15
        )
    except (paramiko.AuthenticationException, paramiko.SSHException, socket.error) as e:
        raise RuntimeError(f"[ERROR] 비밀번호 인증으로 SSH 접속 실패: {e}")

    try:
        # 홈 디렉터리 확인
        stdin, stdout, stderr = ssh.exec_command("echo $HOME")
        home_dir = stdout.read().decode().strip()
        if not home_dir:
            # Windows 일부 환경에서 $HOME 미정의 → PowerShell로 사용자 프로필 경로 조회
            stdin, stdout, stderr = ssh.exec_command(
                "powershell -NoProfile -NonInteractive -Command \"$env:USERPROFILE\""
            )
            home_dir = stdout.read().decode().strip()
        if not home_dir:
            raise RuntimeError("[ERROR] 원격 홈 디렉터리를 확인할 수 없습니다.")

        platform = _detect_remote_platform(ssh)
        ssh_dir, auth_keys = _ensure_ssh_dir_and_perms(ssh, platform, home_dir)

        # 키 추가
        sftp = ssh.open_sftp()
        try:
            changed = _append_key_if_missing(ssh, sftp, auth_keys, public_key_text)
        finally:
            sftp.close()

        # 최종 권한(마무리)
        if platform == "posix":
            ssh.exec_command(f"chmod 700 '{ssh_dir}' && chmod 600 '{auth_keys}'")

        return {"platform": platform, "home": home_dir, "ssh_dir": ssh_dir, "auth_keys": auth_keys, "changed": changed}
    finally:
        ssh.close()

def test_key_login(host, port, username, privkey_path):
    """개인키로 재접속 테스트하고 간단한 명령 실행."""
    pkey = _load_private_key(os.path.expanduser(privkey_path))
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(
            host, port=port, username=username, pkey=pkey,
            look_for_keys=False, allow_agent=False, timeout=15, banner_timeout=15, auth_timeout=15
        )
    except Exception as e:
        raise RuntimeError(f"[ERROR] 키 인증 SSH 접속 실패: {e}")

    try:
        # 플랫폼 감지 후, 사용자/호스트 확인 명령
        platform = _detect_remote_platform(ssh)
        if platform == "posix":
            cmd = "echo OK:$(whoami) on $(hostname)"
        else:
            cmd = "powershell -NoProfile -NonInteractive -Command \"Write-Output ('OK:'+ $env:USERNAME +' on '+ $env:COMPUTERNAME)\""

        _stdin, _stdout, _stderr = ssh.exec_command(cmd)
        exit_code = _stdout.channel.recv_exit_status()
        out = _stdout.read().decode(errors="replace").strip()
        err = _stderr.read().decode(errors="replace").strip()
        return {"exit": exit_code, "stdout": out, "stderr": err, "platform": platform}
    finally:
        ssh.close()

def main():
    ap = argparse.ArgumentParser(description="Deploy SSH public key and immediately test key-based login.")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True, help="초기 1회 비밀번호(키 배포용)")
    ap.add_argument("--pubkey", required=True, help="로컬 공개키 경로 (e.g., ~/.ssh/id_ed25519.pub)")
    ap.add_argument("--privkey", required=True, help="로컬 개인키 경로 (e.g., ~/.ssh/id_ed25519)")
    args = ap.parse_args()

    print(f"[STEP 1] 배포 시작 → {args.user}@{args.host}:{args.port}")
    info = deploy_key(args.host, args.port, args.user, args.password, args.pubkey)
    print(f"[INFO ] 플랫폼: {info['platform']}, 홈: {info['home']}")
    print(f"[INFO ] SSH 디렉터리: {info['ssh_dir']}")
    print(f"[INFO ] authorized_keys: {info['auth_keys']}")
    print(f"[INFO ] 키 {'추가됨' if info['changed'] else '이미 등록되어 있었음'}")

    print(f"[STEP 2] 키 인증 재접속 테스트")
    result = test_key_login(args.host, args.port, args.user, args.privkey)
    print(f"[INFO ] 플랫폼: {result['platform']}")
    print(f"[INFO ] exit={result['exit']}")
    if result["stdout"]:
        print(f"[STDOUT]\n{result['stdout']}")
    if result["stderr"]:
        print(f"[STDERR]\n{result['stderr']}")

    if result["exit"] == 0 and result["stdout"].startswith("OK:"):
        print("[OK   ] 키 인증 접속 테스트 성공 (암호 프롬프트 없이 접속됨).")
    else:
        print("[WARN ] 접속은 되었으나 기대 출력과 다릅니다. 수동 SSH로도 확인해 보세요.")

if __name__ == "__main__":
    main()
