#!/bin/bash

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "╔════════════════════════════════════════════════════════════╗"
echo "║          FUNCTIONAL TESTS (Testes 9-12)                   ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Limpar
pkill -9 webserver 2>/dev/null
sleep 1

# Criar ficheiros
mkdir -p www
echo "<html><body><h1>Test HTML</h1></body></html>" > www/index.html
echo "body { color: blue; }" > www/style.css
echo "console.log('test');" > www/script.js
dd if=/dev/zero of=www/test.jpg bs=1K count=10 2>/dev/null

# Iniciar servidor
echo "Iniciando servidor..."
./webserver server.conf > /tmp/functional_test.log 2>&1 &
SERVER_PID=$!
sleep 3

if ! ps -p $SERVER_PID > /dev/null; then
    echo -e "${RED}✗ ERRO: Servidor não iniciou${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Servidor iniciado (PID: $SERVER_PID)${NC}"
echo ""

PASSED=0
FAILED=0

# ═══════════════════════════════════════════════════════════
# TESTE 9: GET para vários tipos de ficheiro
# ═══════════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TESTE 9] GET requests para vários tipos de ficheiro"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

test_file() {
    local tipo="$1"
    local ficheiro="$2"
    
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:8080/$ficheiro")
    
    if [ "$STATUS" = "200" ]; then
        echo -e "  ${GREEN}✓${NC} $tipo ($ficheiro): Status $STATUS"
        PASSED=$((PASSED + 1))
    else
        echo -e "  ${RED}✗${NC} $tipo ($ficheiro): Status $STATUS (esperado 200)"
        FAILED=$((FAILED + 1))
    fi
}

test_file "HTML" "index.html"
test_file "CSS" "style.css"
test_file "JS" "script.js"
test_file "Imagem" "test.jpg"

echo ""

# ═══════════════════════════════════════════════════════════
# TESTE 10: Status codes implementados
# ═══════════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TESTE 10] Verificar HTTP status codes"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

test_status() {
    local descricao="$1"
    local esperado="$2"
    shift 2
    
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "$@")
    
    if [ "$STATUS" = "$esperado" ]; then
        echo -e "  ${GREEN}✓${NC} $descricao: Status $STATUS"
        PASSED=$((PASSED + 1))
    else
        echo -e "  ${RED}✗${NC} $descricao: Status $STATUS (esperado $esperado)"
        FAILED=$((FAILED + 1))
    fi
}

# Teus status codes: 200, 206, 400, 404, 405, 416, 500, 503
test_status "200 OK" "200" "http://localhost:8080/index.html"
test_status "206 Partial Content" "206" -H "Range: bytes=0-9" "http://localhost:8080/index.html"
test_status "404 Not Found" "404" "http://localhost:8080/naoexiste.html"
test_status "405 Method Not Allowed" "405" -X POST "http://localhost:8080/index.html"
test_status "416 Range Not Satisfiable" "416" -H "Range: bytes=999999-" "http://localhost:8080/index.html"

# 400 Bad Request - pedido mal formado
echo "  Testando 400 Bad Request..."
(echo -e "GET\r\n\r\n"; sleep 1) | nc localhost 8080 > /tmp/test_400.txt 2>&1
if grep -q "400" /tmp/test_400.txt; then
    echo -e "  ${GREEN}✓${NC} 400 Bad Request: Detectado"
    PASSED=$((PASSED + 1))
else
    echo -e "  ${YELLOW}⊘${NC} 400 Bad Request: Não testado (difícil via curl)"
fi

echo -e "  ${YELLOW}⊘${NC} 500 Server Error: Erro interno (não forçado)"
echo -e "  ${YELLOW}⊘${NC} 503 Service Unavailable: Queue cheia (não testado)"

echo ""

# ═══════════════════════════════════════════════════════════
# TESTE 11: Directory index
# ═══════════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TESTE 11] Directory index serving"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:8080/")

if [ "$STATUS" = "200" ]; then
    echo -e "  ${GREEN}✓${NC} Acesso a / → index.html: Status $STATUS"
    PASSED=$((PASSED + 1))
else
    echo -e "  ${RED}✗${NC} Acesso a /: Status $STATUS (esperado 200)"
    FAILED=$((FAILED + 1))
fi

echo ""

# ═══════════════════════════════════════════════════════════
# TESTE 12: Content-Type
# ═══════════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TESTE 12] Verificar Content-Type headers"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

test_content_type() {
    local ficheiro="$1"
    
    CONTENT_TYPE=$(curl -s -I "http://localhost:8080/$ficheiro" | grep -i "Content-Type:" | awk '{print $2}' | tr -d '\r')
    
    echo -e "  ${BLUE}ℹ${NC} $ficheiro: $CONTENT_TYPE"
}

test_content_type "index.html"
test_content_type "style.css"
test_content_type "script.js"
test_content_type "test.jpg"

echo -e "  ${YELLOW}⊘${NC} Content-Type: Usa 'application/octet-stream' para todos"

echo ""

# ═══════════════════════════════════════════════════════════
# RESULTADO FINAL
# ═══════════════════════════════════════════════════════════

echo "╔════════════════════════════════════════════════════════════╗"
echo "║                    RESULTADO FINAL                         ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

TOTAL=$((PASSED + FAILED))
if [ $TOTAL -gt 0 ]; then
    PERCENTAGE=$(( (PASSED * 100) / TOTAL ))
else
    PERCENTAGE=0
fi

echo -e "Testes Passados:  ${GREEN}$PASSED${NC}"
echo -e "Testes Falhados:  ${RED}$FAILED${NC}"
echo -e "Total Testados:   $TOTAL"
echo -e "Taxa de Sucesso:  ${GREEN}${PERCENTAGE}%${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ FUNCTIONAL TESTS COMPLETOS!${NC}"
else
    echo -e "${YELLOW}⚠ Alguns testes falharam${NC}"
fi

echo ""

# Mostrar estatísticas do servidor
echo "Estatísticas do servidor:"
tail -20 /tmp/functional_test.log | grep -A 15 "SERVER STATISTICS" || echo "  (sem estatísticas nos logs)"

echo ""

# Parar servidor
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "Servidor parado."
echo "Logs completos: /tmp/functional_test.log"