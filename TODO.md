# TODO: Implementação do Sistema de Ícone Padrão do Sistema para Pastas

## ✅ Concluído
- [x] Adicionar declarações de funções em FolderColorize.h
- [x] Implementar SetSystemDefaultFolderIcon e RestoreSystemDefaultFolderIcon em FolderColorize.cpp
- [x] Adicionar IDs de controles em resource.h
- [x] Adicionar botões no Controller.rc
- [x] Implementar handlers no DlgProc em main.cpp
- [x] Ajustar tamanho da dialog
- [x] Compilar projeto com sucesso

## 🧪 Testes Pendentes
- [ ] Testar botão "Set Default Icon" - deve abrir picker de arquivo e definir ícone do sistema
- [ ] Testar botão "Restore Default" - deve restaurar ícone padrão do sistema
- [ ] Testar botão "Restart Explorer" - deve reiniciar o Explorer para aplicar mudanças
- [ ] Verificar se mudanças persistem após reboot
- [ ] Testar em diferentes versões do Windows (7, 8, 10, 11)

## 📋 Funcionalidades Implementadas
- **SetSystemDefaultFolderIcon**: Modifica HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons para definir ícone personalizado
- **RestoreSystemDefaultFolderIcon**: Remove a chave do registro para restaurar padrão
- **Botão Restart Explorer**: Usa comando não-documentado ou taskkill/start para reiniciar Explorer
- **UI Integrada**: Botões adicionados ao dialog do instalador com layout apropriado

## 🔧 Melhorias Futuras
- Melhorar seleção de ícone (atualmente usa índice 0, poderia mostrar preview)
- Adicionar validação de arquivos de ícone
- Suporte a mais formatos de ícone
- Logging de operações de registro</content>
<parameter name="filePath">d:\GIT\Folcolor\TODO.md