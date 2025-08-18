#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
JSON 설정 파일에서 서버 목록을 읽어 SSH 공개키를 배포하고,
즉시 개인키 인증 접속 테스트까지 수행합니다.

사용법:
  python deploy_and_test_key.py --config workers_list.json
  (기본값: ./workers_list.json)

JSON 포맷(샘플):
{
  "default_key_path": "~/.ssh/id_ed25519.pub",   // 과거 키 이름도 지원
  "default_pubkey_path": "~/.ssh/id_ed25519.pub",
  "default_privkey_path": "~/.ssh/id_ed25519",
  "servers": [
    {
      "host": "10.10.10.13",
      "port": 22,
      "username": "kiinbang",
      "password": "BangKim_062"
      // 필요시 개별 오버라이드:
      // "pubkey": "~/.ssh/id_ed25519.pub",
      // "privkey": "~/.ssh/id_ed25519",
      // "skip_test": false
    }
  ]
}
"""

import argparse
import json
import os
import posixpath
import socket
import sys
import paramiko

# ---------- 기존 유틸 함수들 (원본 유지/보완) ----------

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
    return "windows"

def _ensure_ssh_dir_and_perms(ssh, platform, home_dir):
    """~/.ssh 디렉터리와 권한 설정 보정."""
    ssh_dir = posixpath.join(home_dir, ".ssh")
    auth_keys = posixpath.join(ssh_dir, "authorized_keys")

    if platform == "posix":
        ssh.exec_command(f"mkdir -p '{ssh_dir}' && chmod 700 '{ssh_dir}'")
        ssh.exec_command(f"touch '{auth_keys}' && chmod 600 '{auth_keys}'")
    else:
        ps = rf"""
$ErrorActionPreference='Stop'
$ssh='{ssh_dir}'
$auth='{auth_keys}'
if (!(Test-Path $ssh)) {{ New-Item -ItemType Directory -Path $ssh | Out-Null }}
if (!(Test-Path $auth)) {{ New-Item -ItemType File -Path $auth | Out-Null }}
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
    """1) 비밀번호 인증 접속 → 2) 키 등록 → 3) 권한 보정"""
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
        stdin, stdout, stderr = ssh.exec_command("echo $HOME")
        home_dir = stdout.read().decode().strip()
        if not home_dir:
            stdin, stdout, stderr = ssh.exec_command(
                "powershell -NoProfile -NonInteractive -Command \"$env:USERPROFILE\""
            )
            home_dir = stdout.read().decode().strip()
        if not home_dir:
            raise RuntimeError("[ERROR] 원격 홈 디렉터리를 확인할 수 없습니다.")

        platform = _detect_remote_platform(ssh)
        ssh_dir, auth_keys = _ensure_ssh_dir_and_perms(ssh, platform, home_dir)

        sftp = ssh.open_sftp()
        try:
            changed = _append_key_if_missing(ssh, sftp, auth_keys, public_key_text)
        finally:
            sftp.close()

        if platform == "posix":
            ssh.exec_command(f"chmod 700 '{ssh_dir}' && chmod 600 '{auth_keys}'")

        return {"platform": platform, "home": home_dir, "ssh_dir": ssh_dir, "auth_keys": auth_keys, "changed": changed}
    finally:
        ssh.close()

def test_key_login(host, port, username, privkey_path):
    """개인키로 재접속 테스트하고 간단한 명령 실행."""
    privkey_path = os.path.expanduser(privkey_path)
    pkey = _load_private_key(privkey_path)
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

# ---------- JSON 기반 메인 로직 ----------

def _load_config(path):
    """BOM 포함 가능성 고려하여 UTF-8-SIG로 로드."""
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)

def _resolve_pub_priv_paths(cfg, srv):
    """
    우선순위:
    1) 서버별 오버라이드: srv['pubkey'], srv['privkey']
    2) 전역 기본값: cfg['default_pubkey_path'] 또는 cfg['default_key_path'], cfg['default_privkey_path']
    3) privkey는 pubkey에서 .pub 제거하여 유도
    """
    pub = srv.get("pubkey") or cfg.get("default_pubkey_path") or cfg.get("default_key_path")
    if not pub:
        raise ValueError("공개키 경로가 누락되었습니다. 'pubkey' 또는 'default_pubkey_path'(또는 'default_key_path')를 지정하세요.")
    priv = srv.get("privkey") or cfg.get("default_privkey_path")
    if not priv and pub.endswith(".pub"):
        priv = pub[:-4]  # ".pub" 제거
    return os.path.expanduser(pub), os.path.expanduser(priv) if priv else None

def main():
    ap = argparse.ArgumentParser(description="Deploy SSH public key(s) from a JSON config and test key-based login.")
    ap.add_argument("--config", default="workers_list.json", help="JSON 설정 파일 경로 (기본: workers_list.json)")
    args = ap.parse_args()

    try:
        cfg = _load_config(args.config)
    except Exception as e:
        print(f"[FATAL] JSON 설정 로드 실패: {e}")
        sys.exit(2)

    servers = cfg.get("servers", [])
    if not isinstance(servers, list) or not servers:
        print("[FATAL] 'servers' 배열이 비어있거나 잘못되었습니다.")
        sys.exit(2)

    any_error = False

    for i, srv in enumerate(servers, 1):
        host = srv.get("host")
        port = int(srv.get("port", 22))
        username = srv.get("username")
        password = srv.get("password")
        skip_test = bool(srv.get("skip_test", False))

        try:
            pubkey_path, privkey_path = _resolve_pub_priv_paths(cfg, srv)
        except Exception as e:
            print(f"[{i:02d}] {host}:{port} 구성 오류 → {e}")
            any_error = True
            continue

        if not all([host, username, password]):
            print(f"[{i:02d}] {host}:{port} 구성 누락 → host/username/password를 확인하세요.")
            any_error = True
            continue

        print(f"\n[STEP 1-{i}] 배포 시작 → {username}@{host}:{port}")
        try:
            info = deploy_key(host, port, username, password, pubkey_path)
            print(f"[INFO ] 플랫폼: {info['platform']}, 홈: {info['home']}")
            print(f"[INFO ] SSH 디렉터리: {info['ssh_dir']}")
            print(f"[INFO ] authorized_keys: {info['auth_keys']}")
            print(f"[INFO ] 키 {'추가됨' if info['changed'] else '이미 등록되어 있었음'}")
        except Exception as e:
            print(f"[ERROR] 키 배포 실패({host}) → {e}")
            any_error = True
            continue

        if skip_test:
            print(f"[STEP 2-{i}] 키 인증 재접속 테스트 건너뜀(skip_test=true).")
            continue

        if not privkey_path:
            print(f"[WARN ] 개인키 경로를 유도하지 못했습니다. (pubkey에서 .pub 제거 방식 사용 또는 default_privkey_path 지정 필요)")
            any_error = True
            continue

        print(f"[STEP 2-{i}] 키 인증 재접속 테스트 → {privkey_path}")
        try:
            result = test_key_login(host, port, username, privkey_path)
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
        except Exception as e:
            print(f"[ERROR] 키 인증 테스트 실패({host}) → {e}")
            any_error = True

    sys.exit(1 if any_error else 0)

if __name__ == "__main__":
    main()
